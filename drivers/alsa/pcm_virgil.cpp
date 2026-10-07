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

#include <cerrno>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "virgil/client.h"
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
  uint64_t hw = 0;           // frames consumed (playback) / produced (capture)
  uint64_t appl = 0;         // frames written (playback) / read (capture)
  uint32_t lead = 0;         // playback: start offset ahead of the media clock
  std::vector<float> prefill;  // playback data written before start()
  std::vector<float> scratch;  // one frame, device channel count
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

bool media_now(VirgilPcm* p, uint64_t* out) {
  double f;
  if (!p->client.frame_now(&f)) return false;
  *out = uint64_t(f);
  return true;
}

// Advance hw from the media clock. Returns false on xrun / daemon loss.
bool update_hw(VirgilPcm* p) {
  if (!p->running) return !p->xrun;
  uint64_t now;
  if (!p->client.daemon_alive() || !media_now(p, &now)) return false;
  p->client.touch();
  if (p->io.stream == SND_PCM_STREAM_PLAYBACK) {
    // A frame counts as consumed once its media time is within one daemon
    // packet of now: after that it can no longer make it onto the wire.
    const uint64_t horizon = now + p->client.period_frames();
    p->hw = horizon > p->start_frame ? horizon - p->start_frame : 0;
    if (p->hw > p->appl) p->xrun = true;
  } else {
    const uint64_t ready = now - p->client.rx_latency_frames();
    p->hw = ready > p->start_frame ? ready - p->start_frame : 0;
    if (p->hw - p->appl > p->io.buffer_size) p->xrun = true;
  }
  return !p->xrun;
}

int virgil_start(snd_pcm_ioplug_t* io) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  uint64_t now;
  if (!ensure_client(p) || !media_now(p, &now)) return -ENODEV;
  const uint32_t ch = device_channels(p);
  if (io->stream == SND_PCM_STREAM_PLAYBACK) {
    p->lead = p->client.tx_lead_frames() + p->client.period_frames();
    p->start_frame = now + p->lead;
    // Flush the prefill into the ring at its final media positions.
    const uint64_t n = std::min<uint64_t>(p->appl, io->buffer_size);
    for (uint64_t k = p->appl - n; k < p->appl; ++k)
      std::memcpy(p->client.tx_frame(p->start_frame + k),
                  &p->prefill[size_t(k % io->buffer_size) * ch], sizeof(float) * ch);
    p->client.set_tx_active(true);
  } else {
    p->start_frame = now - p->client.rx_latency_frames();
  }
  p->hw = 0;
  p->running = true;
  p->xrun = false;
  return 0;
}

int virgil_stop(snd_pcm_ioplug_t* io) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  p->running = false;
  if (io->stream == SND_PCM_STREAM_PLAYBACK && p->client.is_open()) p->client.set_tx_active(false);
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
  const uint32_t ch = device_channels(p);
  const uint32_t n = std::min<uint32_t>(io->channels, ch);

  if (io->stream == SND_PCM_STREAM_PLAYBACK) {
    const uint64_t too_late = p->running ? p->client.tx_horizon() : 0;
    for (snd_pcm_uframes_t f = 0; f < size; ++f) {
      const uint64_t k = p->appl + f;
      float* dst;
      if (!p->running) {
        dst = &p->prefill[size_t(k % io->buffer_size) * ch];
      } else {
        const uint64_t media = p->start_frame + k;
        // Never write behind the daemon: the slot would replay a ring later.
        if (media < too_late) continue;
        dst = p->client.tx_frame(media);
      }
      for (uint32_t c = 0; c < n; ++c) dst[c] = load_sample(areas[c], offset + f, io->format);
      for (uint32_t c = n; c < ch; ++c) dst[c] = 0.f;
    }
  } else {
    for (snd_pcm_uframes_t f = 0; f < size; ++f) {
      const float* src = p->client.rx_frame(p->start_frame + p->appl + f);
      for (uint32_t c = 0; c < io->channels; ++c)
        store_sample(areas[c], offset + f, io->format, c < n ? src[c] : 0.f);
    }
  }
  p->appl += size;
  return snd_pcm_sframes_t(size);
}

int virgil_prepare(snd_pcm_ioplug_t* io) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  if (!ensure_client(p, 3000)) return -ENODEV;
  p->running = false;
  p->xrun = false;
  p->hw = p->appl = 0;
  if (io->stream == SND_PCM_STREAM_PLAYBACK) {
    p->prefill.assign(size_t(io->buffer_size) * p->client.tx_channels(), 0.f);
    p->client.set_tx_active(false);
  }
  // Wake the application once per period.
  const int64_t ns = int64_t(io->period_size) * 1000000000LL / io->rate;
  itimerspec its{};
  its.it_interval.tv_sec = time_t(ns / 1000000000LL);
  its.it_interval.tv_nsec = long(ns % 1000000000LL);
  its.it_value = its.it_interval;
  timerfd_settime(p->timer_fd, 0, &its, nullptr);
  return 0;
}

// Playback writes run up to buffer_size + lead ahead of the media clock and
// capture reads trail it by buffer_size + latency; both must stay inside half
// the ring.
uint32_t max_buffer_frames(const virgil::Client& c) {
  return c.ring_frames() / 2 - 8 * c.period_frames() - c.rx_latency_frames();
}

int virgil_hw_params(snd_pcm_ioplug_t* io, snd_pcm_hw_params_t*) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  return io->buffer_size <= max_buffer_frames(p->client) ? 0 : -EINVAL;
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
  if (pfd->revents & POLLIN) {
    uint64_t expirations;
    if (read(p->timer_fd, &expirations, sizeof expirations) < 0) { /* spurious */ }
  }
  const unsigned short ready = io->stream == SND_PCM_STREAM_PLAYBACK ? POLLOUT : POLLIN;
  if (!update_hw(p)) {
    // Wake the app so its next read/write reaches pointer(), which reports
    // -EPIPE (xrun). POLLERR here would surface as a fatal -EIO instead of
    // the recoverable xrun that lets it re-prepare after an engine restart.
    *revents = ready;
    return 0;
  }
  uint64_t avail;
  if (io->stream == SND_PCM_STREAM_PLAYBACK)
    avail = io->buffer_size - std::min<uint64_t>(io->buffer_size, p->appl - std::min(p->appl, p->hw));
  else
    avail = p->running ? p->hw - p->appl : 0;
  *revents = avail >= io->period_size ? ready : 0;
  return 0;
}

int virgil_delay(snd_pcm_ioplug_t* io, snd_pcm_sframes_t* delay) {
  auto* p = static_cast<VirgilPcm*>(io->private_data);
  update_hw(p);
  if (io->stream == SND_PCM_STREAM_PLAYBACK)
    *delay = snd_pcm_sframes_t(p->appl - std::min(p->appl, p->hw)) +
             snd_pcm_sframes_t(p->client.period_frames());
  else
    *delay = snd_pcm_sframes_t(p->hw - p->appl) + snd_pcm_sframes_t(p->client.rx_latency_frames());
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
    SNDERR("virgil: daemon not running (start virgild) or no free playback slot");
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
