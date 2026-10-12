// ALSA external PCM plugin: exposes virgild as an ALSA device ("virgil").
//
//   pcm.virgil { type virgil }               # see 50-virgil.conf
//   aplay -D virgil file.wav / arecord -D virgil / jackd -d alsa -d virgil / PipeWire
//
// Playback frames are written straight into this pcm's mixing slot at their
// media-clock position as soon as the application hands them over; the hw
// pointer advances with the PTP-disciplined media clock. Capture reads the
// shared receive ring `rx_latency` behind the clock. No extra buffering.
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "virgil/client.h"
#include "virgil/platform.h"
#include "virgil/sample_convert.h"

namespace {

struct VirgilPcm {
  snd_pcm_ioplug_t io{};
  virgil::Client client;
  std::string shm_name;
  int timer_fd = -1;
  bool running = false;
  bool xrun = false;
  uint64_t start_frame = 0;  // media frame of stream position 0
  uint64_t clock_steps = 0;  // daemon's clock_steps when the stream started
  uint64_t hw = 0;           // frames consumed (playback) / produced (capture)
  uint64_t appl = 0;         // frames written (playback) / read (capture),
                             // unwrapped mirror of io.appl_ptr (see sync_appl)
  snd_pcm_uframes_t last_appl_ptr = 0;  // io.appl_ptr that `appl` reflects
  snd_pcm_uframes_t boundary = 0;       // wrap point of io.appl_ptr
  uint32_t lead = 0;         // playback: start offset ahead of the media clock
  std::vector<float> prefill;  // playback data written before start()
  uint32_t prefill_ch = 0;     // device channel count `prefill` is laid out for
  std::vector<float> scratch;  // one frame, device channel count
  bool small_buffer_hinted = false;  // hint at most once per open
  snd_pcm_uframes_t avail_min = 0;   // from sw_params; 0 = period_size
  // Last good clock anchor of this stream's timeline: a read that races the
  // daemon publishing a new one falls back to it instead of an xrun.
  virgil::Client::AnchorCache anchor_cache;
};

// (Re)attach to virgild. `wait_ms` rides out an engine restart (settings applied
// from the control panel), so applications only see an xrun, not an error.
bool ensure_client(VirgilPcm* p, int wait_ms = 0) {
  for (int waited = 0;; waited += 50) {
    if (p->client.is_open() && p->client.daemon_alive()) {
      if (p->io.stream != SND_PCM_STREAM_PLAYBACK || p->client.has_tx_slot()) return true;
      return p->client.acquire_tx_slot("alsa");
    }
    // Not alive. If a live soundcard exists under our name (daemon restarted
    // with a new layout, or after a crash) switch to it; if it is the same
    // segment coming back (settings applied) we simply keep waiting on it.
    if (p->client.is_open()) {
      virgil::Client fresh;
      if (fresh.open(p->shm_name) && fresh.daemon_alive()) {
        p->client.close();
        continue;
      }
    } else if (p->client.open(p->shm_name)) {
      continue;
    }
    if (waited >= wait_ms) return false;
    timespec ts{0, 50 * 1000000L};
    nanosleep(&ts, nullptr);
  }
}

uint32_t device_channels(const VirgilPcm* p) {
  return p->io.stream == SND_PCM_STREAM_PLAYBACK ? p->client.tx_channels()
                                                 : p->client.rx_channels();
}

// alsa-lib wraps appl_ptr / hw_ptr at pcm->boundary, which is not exported to
// ioplug plugins; it is derived from buffer_size the same way alsa-lib does.
snd_pcm_uframes_t compute_boundary(snd_pcm_uframes_t buffer_size) {
  const snd_pcm_uframes_t lmax = snd_pcm_uframes_t(LONG_MAX);
  snd_pcm_uframes_t b = buffer_size;
  if (!b) return 0;
  while (b * 2 <= lmax - buffer_size) b *= 2;
  return b;
}

// Bring `appl` in line with io->appl_ptr. snd_pcm_rewind / snd_pcm_forward
// move appl_ptr without any plugin callback, so every position computation
// starts from appl_ptr rather than from what transfer() has seen.
void sync_appl(VirgilPcm* p) {
  const snd_pcm_uframes_t cur = p->io.appl_ptr;
  int64_t d = int64_t(cur) - int64_t(p->last_appl_ptr);
  if (p->boundary) {
    const int64_t b = int64_t(p->boundary);
    if (d < -b / 2) d += b;
    else if (d > b / 2) d -= b;
  }
  // A rewind can never reach before stream position 0.
  p->appl = d < 0 && uint64_t(-d) > p->appl ? 0 : p->appl + uint64_t(d);
  p->last_appl_ptr = cur;
}

// Sample at (area, frame) <-> float.
inline float load_sample(const snd_pcm_channel_area_t& a, snd_pcm_uframes_t frame,
                         snd_pcm_format_t fmt) {
  const uint8_t* p =
      static_cast<const uint8_t*>(a.addr) + (a.first + size_t(frame) * a.step) / 8;
  switch (fmt) {
    case SND_PCM_FORMAT_FLOAT_LE: { float v; std::memcpy(&v, p, 4); return v; }
    case SND_PCM_FORMAT_S32_LE: { int32_t v; std::memcpy(&v, p, 4); return virgil::s32_to_float(v); }
    case SND_PCM_FORMAT_S24_LE: {
      int32_t v; std::memcpy(&v, p, 4);
      return float(int32_t(uint32_t(v) << 8) >> 8) * (1.0f / 8388608.0f);
    }
    case SND_PCM_FORMAT_S16_LE: { int16_t v; std::memcpy(&v, p, 2); return virgil::s16_to_float(v); }
    default: return 0.f;
  }
}

inline void store_sample(const snd_pcm_channel_area_t& a, snd_pcm_uframes_t frame,
                         snd_pcm_format_t fmt, float x) {
  uint8_t* p = static_cast<uint8_t*>(a.addr) + (a.first + size_t(frame) * a.step) / 8;
  switch (fmt) {
    case SND_PCM_FORMAT_FLOAT_LE: std::memcpy(p, &x, 4); break;
    case SND_PCM_FORMAT_S32_LE: { int32_t v = virgil::float_to_s32(x); std::memcpy(p, &v, 4); break; }
    case SND_PCM_FORMAT_S24_LE: { int32_t v = virgil::float_to_int(x, 1 << 23); std::memcpy(p, &v, 4); break; }
    case SND_PCM_FORMAT_S16_LE: { int16_t v = virgil::float_to_s16(x); std::memcpy(p, &v, 2); break; }
    default: break;
  }
}

// `steps`: the daemon's clock_steps, read before this call.
bool media_now(VirgilPcm* p, uint64_t steps, uint64_t* out) {
  double f;
  if (!p->client.frame_now(&f, steps, &p->anchor_cache)) return false;
  *out = uint64_t(f);
  return true;
}

snd_pcm_uframes_t avail_min(const VirgilPcm* p) {
  return p->avail_min ? p->avail_min : p->io.period_size;
}

// The poll timer. While running it is a one-shot at the media-clock moment
// the application's wait ends: avail reaching avail_min (a drain: the
// buffer played out). It follows the PTP rate and phase, so the app wakes
// with the full buffer - avail_min of margin rather than up to a period
// later at whatever phase a free-running interval timer happens to have.
// Re-armed wherever that moment changes (start, transfer, poll_revents when
// not ready, prepare, stop). Without a clock (not started, xrun, daemon
// gone) it ticks once per Virgil tick, so a waiting app still sees the state
// change.
void arm_timer(VirgilPcm* p) {
  const snd_pcm_ioplug_t* io = &p->io;
  itimerspec its{};
  virgil::ClockAnchor a;
  if (p->running && !p->xrun && p->client.is_open() && io->rate &&
      p->client.anchor(&a, p->clock_steps, &p->anchor_cache)) {
    // `appl` as the caller left it: no sync_appl here, transfer() has
    // already counted the forward alsa-lib makes only after it returns.
    // Media frame at which the wait ends (see update_hw for hw).
    uint64_t target;
    const bool draining = io->stream == SND_PCM_STREAM_PLAYBACK && io->state == SND_PCM_STATE_DRAINING;
    if (draining) {
      // Drained once the daemon's tick has taken the last frame (update_hw):
      // the tick at the first boundary at or after its end.
      const uint64_t P = std::max<uint32_t>(1, p->client.period_frames());
      target = (p->start_frame + p->appl + P - 1) / P * P;
    } else if (io->stream == SND_PCM_STREAM_PLAYBACK) {
      // avail = buffer - (appl - hw) >= avail_min.
      const uint64_t need = uint64_t(io->buffer_size) - std::min<uint64_t>(io->buffer_size, avail_min(p));
      target = p->start_frame + (p->appl > need ? p->appl - need : 0);
    } else {
      // avail = hw - appl >= avail_min.
      target = p->start_frame + p->appl + avail_min(p) + p->client.rx_latency_frames();
    }
    const int64_t now = virgil::mono_ns();
    // host_ns_at truncates: 1 us late so the wake sees the frame reached.
    int64_t at = a.host_ns_at(double(target)) + 1000;
    // Never one long sleep on one anchor: a clock step or a lost daemon
    // must still wake the app within a few ms to report the xrun.
    at = std::min<int64_t>(at, now + 20000000);
    // Already due: fire at once (an absolute time in the past expires
    // immediately). A drain waiting on a tick that is late polls gently.
    if (at <= now) at = draining ? now + 100000 : now;
    if (at <= 0) at = 1;      // it_value 0 would disarm
    its.it_value.tv_sec = time_t(at / 1000000000LL);
    its.it_value.tv_nsec = long(at % 1000000000LL);
    timerfd_settime(p->timer_fd, TFD_TIMER_ABSTIME, &its, nullptr);
    return;
  }
  const uint32_t tick = p->client.is_open() ? p->client.period_frames() : uint32_t(io->period_size);
  const uint32_t rate = io->rate ? io->rate : (p->client.is_open() ? p->client.sample_rate() : 48000);
  int64_t ns = int64_t(tick ? tick : 48) * 1000000000LL / rate;
  if (ns <= 0) ns = 1000000;
  its.it_interval.tv_sec = time_t(ns / 1000000000LL);
  its.it_interval.tv_nsec = long(ns % 1000000000LL);
  its.it_value = its.it_interval;
  timerfd_settime(p->timer_fd, 0, &its, nullptr);
}

// Advance hw from the media clock. Returns false on xrun / daemon loss.
bool update_hw(VirgilPcm* p) {
  sync_appl(p);
  if (!p->running) return !p->xrun;
  uint64_t now;
  if (!p->client.daemon_alive()) return false;
  // The timeline moved (PTP lock, master change, engine restart): positions
  // from before are meaningless, so report an xrun and let the app restart.
  // Read before the anchor (see virgil_start).
  if (p->client.clock_steps() != p->clock_steps) {
    p->xrun = true;
    return false;
  }
  if (!media_now(p, p->clock_steps, &now)) return false;
  p->client.touch();
  if (p->io.stream == SND_PCM_STREAM_PLAYBACK) {
    // A frame counts as consumed once its media time is reached. The daemon
    // takes it at the end of its tick (tx_horizon), up to one tick later, so
    // this is the earliest it can go; never report less than the daemon has.
    const uint64_t consumed = p->client.tx_horizon();
    const uint64_t horizon = std::max<uint64_t>(now, consumed);
    uint64_t hw = horizon > p->start_frame ? horizon - p->start_frame : 0;
    // Draining: the stream stops (and our slot goes inactive, which the
    // daemon no longer mixes) as soon as hw reaches appl, so hold hw short
    // of it until the daemon has really taken the last frame, up to one
    // tick after its media time. hw never goes backwards.
    if (p->io.state == SND_PCM_STATE_DRAINING && p->appl > 0 && consumed < p->start_frame + p->appl)
      hw = std::min(hw, std::max(p->hw, p->appl - 1));
    p->hw = hw;
    if (p->hw > p->appl) p->xrun = true;
  } else {
    const uint64_t ready = now - p->client.rx_latency_frames();
    p->hw = ready > p->start_frame ? ready - p->start_frame : 0;
    if (p->hw > p->appl && p->hw - p->appl > p->io.buffer_size) p->xrun = true;
  }
  return !p->xrun;
}

// Fail start() as an xrun: the application re-prepares (which reconnects and
// re-validates against the daemon) instead of seeing a fatal error.
int start_xrun(snd_pcm_ioplug_t* io) {
  snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
  return -EPIPE;
}

int virgil_start(snd_pcm_ioplug_t* io) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  sync_appl(p);
  // No reconnect here: the prefill and hw params were set up for the client
  // attached at prepare(). If that daemon is gone, let prepare() switch.
  uint64_t now;
  if (!p->client.is_open() || !p->client.daemon_alive() ||
      (io->stream == SND_PCM_STREAM_PLAYBACK && !p->client.has_tx_slot()))
    return start_xrun(io);
  // The step count before the anchor: the daemon counts a step only once
  // the new timeline's anchor is out, so the anchor read next is at least
  // as new as this count (a later step is caught by update_hw).
  const uint64_t steps = p->client.clock_steps();
  p->anchor_cache.valid = false;  // a new stream: no fallback from before
  if (!media_now(p, steps, &now)) return start_xrun(io);
  const uint32_t ch = device_channels(p);
  if (io->stream == SND_PCM_STREAM_PLAYBACK &&
      (ch != p->prefill_ch || p->prefill.size() != size_t(io->buffer_size) * ch))
    return start_xrun(io);
  p->clock_steps = steps;
  if (io->stream == SND_PCM_STREAM_PLAYBACK) {
    p->client.clear_pending_tx();  // nothing from before the last stop may replay
    p->lead = p->client.tx_lead_frames() + p->client.period_frames();
    p->start_frame = now + p->lead;
    // Flush the prefill into the ring at its final media positions.
    const uint64_t n = std::min<uint64_t>(p->appl, io->buffer_size);
    for (uint64_t k = p->appl - n; k < p->appl; ++k)
      std::memcpy(p->client.tx_frame(p->start_frame + k),
                  &p->prefill[size_t(k % io->buffer_size) * ch], sizeof(float) * ch);
    p->client.set_tx_active(true);
    p->client.note_written(p->start_frame + p->appl);
  } else {
    p->start_frame = now - p->client.rx_latency_frames();
  }
  p->hw = 0;
  p->running = true;
  p->xrun = false;
  arm_timer(p);
  return 0;
}

int virgil_stop(snd_pcm_ioplug_t* io) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  p->running = false;
  if (io->stream == SND_PCM_STREAM_PLAYBACK && p->client.is_open()) p->client.set_tx_active(false);
  arm_timer(p);
  return 0;
}

snd_pcm_sframes_t virgil_pointer(snd_pcm_ioplug_t* io) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  if (!update_hw(p)) return -EPIPE;
  return snd_pcm_sframes_t(p->hw % io->buffer_size);
}

snd_pcm_sframes_t virgil_transfer(snd_pcm_ioplug_t* io, const snd_pcm_channel_area_t* areas,
                               snd_pcm_uframes_t offset, snd_pcm_uframes_t size) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  if (!p->client.is_open()) return -ENODEV;
  // io->appl_ptr is the stream position of the first frame of this chunk
  // (alsa-lib advances it after transfer returns), so rewinds and forwards
  // since the last call are picked up here.
  sync_appl(p);
  const uint32_t ch = device_channels(p);
  const uint32_t n = std::min<uint32_t>(io->channels, ch);
  if (io->stream == SND_PCM_STREAM_PLAYBACK && !p->running && ch != p->prefill_ch) return -EPIPE;

  if (io->stream == SND_PCM_STREAM_PLAYBACK) {
    const uint64_t too_late = p->running ? p->client.tx_horizon() : 0;
    uint64_t late = 0;
    for (snd_pcm_uframes_t f = 0; f < size; ++f) {
      const uint64_t k = p->appl + f;
      float* dst;
      if (!p->running) {
        dst = &p->prefill[size_t(k % io->buffer_size) * ch];
      } else {
        const uint64_t media = p->start_frame + k;
        // Never write behind the daemon: the slot would replay a ring later.
        if (media < too_late) {
          ++late;
          continue;
        }
        dst = p->client.tx_frame(media);
      }
      for (uint32_t c = 0; c < n; ++c) dst[c] = load_sample(areas[c], offset + f, io->format);
      for (uint32_t c = n; c < ch; ++c) dst[c] = 0.f;
    }
    if (p->running) {
      p->client.note_late(late);
      p->client.note_written(p->start_frame + p->appl + size);
    }
  } else {
    for (snd_pcm_uframes_t f = 0; f < size; ++f) {
      const float* src = p->client.rx_frame(p->start_frame + p->appl + f);
      for (uint32_t c = 0; c < io->channels; ++c)
        store_sample(areas[c], offset + f, io->format, c < n ? src[c] : 0.f);
    }
  }
  // Account for the forward alsa-lib does once we return.
  p->appl += size;
  p->last_appl_ptr += size;
  if (p->boundary && p->last_appl_ptr >= p->boundary) p->last_appl_ptr -= p->boundary;
  if (p->running) arm_timer(p);
  return snd_pcm_sframes_t(size);
}

uint32_t max_buffer_frames(const virgil::Client& c);

int virgil_prepare(snd_pcm_ioplug_t* io) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  if (!ensure_client(p, 3000)) return -ENODEV;
  // ensure_client may have switched to a restarted virgild. The stream's hw
  // params were negotiated against the old layout; refuse rather than run
  // with mismatched geometry.
  if (p->client.sample_rate() != io->rate || device_channels(p) != io->channels ||
      io->buffer_size > max_buffer_frames(p->client)) {
    SNDERR("virgil: daemon layout changed (rate/channels/buffer); reopen the device");
    return -ENODEV;
  }
  p->running = false;
  p->xrun = false;
  p->hw = p->appl = 0;
  p->boundary = compute_boundary(io->buffer_size);
  p->last_appl_ptr = io->appl_ptr;
  if (io->stream == SND_PCM_STREAM_PLAYBACK) {
    p->prefill_ch = p->client.tx_channels();
    p->prefill.assign(size_t(io->buffer_size) * p->prefill_ch, 0.f);
    p->client.set_tx_active(false);
  }
  p->anchor_cache.valid = false;  // ensure_client may have switched daemons
  arm_timer(p);  // not running: the per-tick fallback until start()
  return 0;
}

// Playback writes run up to buffer_size + lead ahead of the media clock and
// capture reads trail it by buffer_size + latency; both must stay inside half
// the ring.
uint32_t max_buffer_frames(const virgil::Client& c) {
  const int64_t m = int64_t(c.ring_frames() / 2) - 8 * int64_t(c.period_frames()) - int64_t(c.rx_latency_frames());
  return m > 64 ? uint32_t(std::min<int64_t>(m, 1 << 20)) : 64;
}

// A buffer under ~10 ms, or a period under two Virgil ticks, leaves little
// slack for the application's scheduling (on a Raspberry Pi it regularly
// misses its deadline by 8-38 ms). Only a hint: such buffers do work on a
// quiet machine.
void hint_small_buffer(VirgilPcm* p) {
  const snd_pcm_ioplug_t* io = &p->io;
  if (p->small_buffer_hinted || io->rate == 0) return;
  const uint64_t min_buffer = uint64_t(io->rate) / 100;  // 10 ms
  const uint64_t tick = p->client.period_frames();
  if (io->buffer_size >= min_buffer && io->period_size >= 2 * tick) return;
  p->small_buffer_hinted = true;
  SNDERR("virgil: %s buffer of %.1f ms (period %.1f ms, Virgil tick %.1f ms) is small; if you hear "
         "dropouts raise the app's buffer (>= 10 ms on a Raspberry Pi)",
         io->stream == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture",
         1000.0 * double(io->buffer_size) / io->rate, 1000.0 * double(io->period_size) / io->rate,
         1000.0 * double(tick) / io->rate);
}

int virgil_hw_params(snd_pcm_ioplug_t* io, snd_pcm_hw_params_t*) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  if (io->buffer_size > max_buffer_frames(p->client)) return -EINVAL;
  hint_small_buffer(p);
  return 0;
}

int virgil_poll_descriptors_count(snd_pcm_ioplug_t*) { return 1; }

int virgil_poll_descriptors(snd_pcm_ioplug_t* io, pollfd* pfd, unsigned int space) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  if (space < 1) return -EINVAL;
  pfd->fd = p->timer_fd;
  pfd->events = POLLIN;
  pfd->revents = 0;
  return 1;
}

int virgil_poll_revents(snd_pcm_ioplug_t* io, pollfd* pfd, unsigned int nfds,
                     unsigned short* revents) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  if (nfds != 1) return -EINVAL;
  const bool fired = (pfd->revents & POLLIN) != 0;
  auto rearm = [&] {
    uint64_t expirations;
    if (fired && read(p->timer_fd, &expirations, sizeof expirations) < 0) { /* spurious */ }
    arm_timer(p);
  };
  const unsigned short ready = io->stream == SND_PCM_STREAM_PLAYBACK ? POLLOUT : POLLIN;
  if (!update_hw(p)) {
    // Wake the app so its next read/write reaches pointer(), which reports
    // -EPIPE (xrun). POLLERR here would surface as a fatal -EIO instead of
    // the recoverable xrun that lets it re-prepare after an engine restart.
    *revents = ready;
    rearm();
    return 0;
  }
  uint64_t avail;
  if (io->stream == SND_PCM_STREAM_PLAYBACK)
    avail = io->buffer_size - std::min<uint64_t>(io->buffer_size, p->appl - std::min(p->appl, p->hw));
  else
    avail = p->running ? p->hw - std::min(p->appl, p->hw) : 0;
  *revents = avail >= avail_min(p) ? ready : 0;
  if (*revents && p->running && io->state != SND_PCM_STATE_DRAINING) {
    // Ready: level-triggered like a hardware device. The timer stays
    // expired (the fd readable) until transfer() re-arms it for the next
    // wait; one that has not fired yet (revents asked without a poll) is
    // made to fire now.
    if (!fired) arm_timer(p);
  } else {
    rearm();  // not ready yet (or draining): sleep until it will be
  }
  return 0;
}

int virgil_sw_params(snd_pcm_ioplug_t* io, snd_pcm_sw_params_t* params) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  snd_pcm_uframes_t v = 0;
  if (snd_pcm_sw_params_get_avail_min(params, &v) < 0) v = 0;
  p->avail_min = v;
  if (p->running) {
    sync_appl(p);
    arm_timer(p);
  }
  return 0;
}

int virgil_delay(snd_pcm_ioplug_t* io, snd_pcm_sframes_t* delay) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  update_hw(p);
  // Playback: frames until the last one written reaches its media time
  // (hw is the media clock itself, see update_hw).
  if (io->stream == SND_PCM_STREAM_PLAYBACK)
    *delay = snd_pcm_sframes_t(p->appl - std::min(p->appl, p->hw));
  else
    *delay = snd_pcm_sframes_t(p->hw - std::min(p->appl, p->hw)) +
             snd_pcm_sframes_t(p->client.rx_latency_frames());
  return 0;
}

int virgil_close(snd_pcm_ioplug_t* io) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  if (p->timer_fd >= 0) close(p->timer_fd);
  delete p;
  return 0;
}

const snd_pcm_ioplug_callback_t kCallbacks = [] {
  snd_pcm_ioplug_callback_t c{};
  c.start = virgil_start;
  c.stop = virgil_stop;
  c.pointer = virgil_pointer;
  c.transfer = virgil_transfer;
  c.close = virgil_close;
  c.hw_params = virgil_hw_params;
  c.prepare = virgil_prepare;
  c.poll_descriptors_count = virgil_poll_descriptors_count;
  c.poll_descriptors = virgil_poll_descriptors;
  c.poll_revents = virgil_poll_revents;
  c.delay = virgil_delay;
  c.sw_params = virgil_sw_params;
  return c;
}();

int set_constraints(VirgilPcm* p) {
  snd_pcm_ioplug_t* io = &p->io;
  static const unsigned accesses[] = {SND_PCM_ACCESS_RW_INTERLEAVED,
                                      SND_PCM_ACCESS_RW_NONINTERLEAVED,
                                      SND_PCM_ACCESS_MMAP_INTERLEAVED,
                                      SND_PCM_ACCESS_MMAP_NONINTERLEAVED};
  static const unsigned formats[] = {SND_PCM_FORMAT_FLOAT_LE, SND_PCM_FORMAT_S32_LE,
                                     SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S16_LE};
  const unsigned ch = device_channels(p);
  const unsigned rate = p->client.sample_rate();
  // Byte limits are computed for the narrowest format (16-bit) so they never
  // admit more frames than max_buffer_frames().
  const unsigned max_frames = max_buffer_frames(p->client);
  int err;
  if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS, 4, accesses)) < 0 ||
      (err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT, 4, formats)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS, ch, ch)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE, rate, rate)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 2, 64)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 16 * ch * 4,
                                             max_frames / 2 * ch * 2)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 32 * ch * 4,
                                             max_frames * ch * 2)) < 0)
    return err;
  return 0;
}

}  // namespace

extern "C" {

SND_PCM_PLUGIN_DEFINE_FUNC(virgil) {
  (void)root;
  std::string shm_name;
  snd_config_iterator_t i, next;
  snd_config_for_each(i, next, conf) {
    snd_config_t* n = snd_config_iterator_entry(i);
    const char* id;
    if (snd_config_get_id(n, &id) < 0) continue;
    if (!strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint")) continue;
    if (!strcmp(id, "shm")) {
      const char* s;
      if (snd_config_get_string(n, &s) < 0) {
        SNDERR("virgil: 'shm' must be a string");
        return -EINVAL;
      }
      shm_name = s;
      continue;
    }
    SNDERR("virgil: unknown field %s", id);
    return -EINVAL;
  }

  auto* p = new VirgilPcm;
  p->shm_name = shm_name;
  p->io.version = SND_PCM_IOPLUG_VERSION;
  p->io.name = "Virgil Virtual Soundcard (Dante)";
  p->io.callback = &kCallbacks;
  p->io.private_data = p;
  p->io.mmap_rw = 0;

  int err;
  p->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (p->timer_fd < 0) {
    delete p;
    return -errno;
  }
  // stream must be known before ensure_client decides on a tx slot.
  p->io.stream = stream;
  if (!ensure_client(p)) {
    if (!p->client.is_open())
      SNDERR("virgil: %s", p->client.open_error().empty() ? "daemon not running (start virgild)"
                                                          : p->client.open_error().c_str());
    else if (!p->client.daemon_alive())
      SNDERR("virgil: %s", p->client.header()->state.load() == virgil::kStateStopped
                               ? "Virgil is stopped: press Start in Virgil Control"
                               : "the Virgil daemon is not responding: restart virgild");
    else
      SNDERR("virgil: no free playback slot (all Virgil playback slots are in use)");
    close(p->timer_fd);
    delete p;
    return -ENODEV;
  }
  p->io.poll_fd = p->timer_fd;
  p->io.poll_events = POLLIN;
  if ((err = snd_pcm_ioplug_create(&p->io, name, stream, mode)) < 0) {
    close(p->timer_fd);
    delete p;
    return err;
  }
  if ((err = set_constraints(p)) < 0) {
    snd_pcm_ioplug_delete(&p->io);
    return err;
  }
  *pcmp = p->io.pcm;
  return 0;
}

SND_PCM_PLUGIN_SYMBOL(virgil);

}  // extern "C"
