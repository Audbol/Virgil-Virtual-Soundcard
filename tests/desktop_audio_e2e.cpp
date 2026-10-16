// Desktop audio end to end on Linux with a real PulseAudio / PipeWire server
// (run by tests/desktop_audio_e2e.sh against a private PulseAudio with null
// sinks; not part of ctest, which has no sound server).
//
//   virgil_desktop_audio_e2e list
//       output devices and apps playing, as the Desktop audio window lists them
//   virgil_desktop_audio_e2e probe --source SPEC [--seconds S]
//       one capture stream on its own: callback sizes and gaps
//   virgil_desktop_audio_e2e tx --shm NAME --source SPEC [--channel N] [--mono]
//                               [--seconds S] [--freq HZ] [--share F]
//       desktop audio into a running virgild (free-run): checks that a tone
//       arrives in the "Desktop audio" playback slot at the mapped channels
//       (more than F of their power at HZ, default 0.95) and nowhere else
//   virgil_desktop_audio_e2e loop --source SPEC --out SINK [--seconds S]
//                                 [--ppm P] [--channel N]
//       an in-process Virgil engine whose Dante transmit channels are looped
//       back to its receive channels (as tests/loopback_test.cpp does), with
//       its media clock P ppm off the system clock: SPEC -> Virgil tx ch N/N+1
//       -> rx ch N/N+1 -> output device SINK; --measure DEV also times tone
//       bursts played on DEV against their arrival on SINK (latency)
//
//   virgil_desktop_audio_e2e inputs --shm NAME --fifo PATH --dev-ch D --source input:<source>
//                                   --first F --count N --channel C [--seconds S]
//       an audio interface's inputs into a running virgild (free-run): this
//       program feeds a module-pipe-source (PATH, D float32 channels, input k
//       = +-0.05 k, the sign flipping for 100 ms every second) in real time;
//       its inputs F.. go to transmit channels C.. Checks the values that
//       arrive in the "Desktop audio" slot (exact, the others silent) and the
//       latency from writing a flip to its Dante transmit time.
//   virgil_desktop_audio_e2e outputs --out SINK --dev-ch D --first F --count N
//                                    --channel C [--seconds S]
//       an in-process Virgil engine whose receive channel k carries +-0.1 k
//       (sign flipping for 100 ms every second of media time); receive
//       channels C.. go to the outputs F.. of SINK (D channels), recorded with
//       parec from its monitor: exact values there, silence on the other
//       outputs, and the latency from Dante media time to the device.
//
//   SPEC: device:<sink>|device:default|app:<name>|all|input:<source>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "desktop_audio_engine.h"
#include "virgil/client.h"
#include "virgil/engine.h"
#include "virgil/log.h"
#include "virgil/media_clock.h"
#include "virgil/platform.h"

namespace {

void wait_ms(int ms) { virgil::sleep_until_ns(virgil::mono_ns() + int64_t(ms) * 1000000); }

struct Args {
  std::string mode, shm, source = "device:default", out = "default", measure_src, fifo;
  unsigned channel = 1, first = 1, count = 2, dev_ch = 0;
  bool mono = false;
  double seconds = 4, freq = 1000, ppm = 0, share = 0.95;
};

bool parse_source(const std::string& spec, vc::DesktopAudioSource* s) {
  if (spec == "all") {
    s->type = vc::DesktopAudioSource::kAllApps;
  } else if (spec.rfind("device:", 0) == 0) {
    s->type = vc::DesktopAudioSource::kDevice;
    s->device = spec.substr(7);
  } else if (spec.rfind("input:", 0) == 0) {
    s->type = vc::DesktopAudioSource::kInput;
    s->device = spec.substr(6);
  } else if (spec.rfind("app:", 0) == 0) {
    s->type = vc::DesktopAudioSource::kApp;
    s->app = spec.substr(4);
  } else {
    return false;
  }
  return true;
}

void print_status(desk::Engine& e) {
  const desk::Status st = e.status();
  std::printf("  [%5.1f s] %s\n", virgil::mono_ns() / 1e9, st.text.c_str());
  for (size_t i = 0; i < st.sources.size(); ++i)
    std::printf("      source %zu: %s%s (peak %.3f)\n", i + 1, st.sources[i].error ? "ERROR " : "",
                st.sources[i].text.c_str(), double(st.sources[i].peak));
  if (!st.output.text.empty() && st.output.text != "Off")
    std::printf("      output: %s%s (peak %.3f)\n", st.output.error ? "ERROR " : "", st.output.text.c_str(),
                double(st.output.peak));
}

// Power of `freq` in `x` relative to all power (0..1), and the RMS.
void analyse(const std::vector<float>& x, double rate, double freq, double* rms, double* tone_share) {
  double e = 0;
  for (float v : x) e += double(v) * v;
  *rms = x.empty() ? 0 : std::sqrt(e / double(x.size()));
  const double w = 2 * M_PI * freq / rate;
  double re = 0, im = 0;
  for (size_t n = 0; n < x.size(); ++n) {
    const double hann = 0.5 - 0.5 * std::cos(2 * M_PI * double(n) / double(x.size()));
    re += hann * x[n] * std::cos(w * double(n));
    im -= hann * x[n] * std::sin(w * double(n));
  }
  // Hann window: a full-scale sine of amplitude A gives |X| = A N / 4, power A^2/2.
  const double amp = 4 * std::sqrt(re * re + im * im) / double(std::max<size_t>(1, x.size()));
  *tone_share = e > 0 ? std::min(1.0, (amp * amp / 2) / (e / double(x.size()))) : 0;
}

vc::DesktopAudioSettings settings_for(const Args& a, bool output) {
  vc::DesktopAudioSettings s;
  s.enabled = true;
  vc::DesktopAudioSource src;
  if (!parse_source(a.source, &src)) {
    std::fprintf(stderr, "bad --source %s\n", a.source.c_str());
    std::exit(2);
  }
  src.channel = a.channel;
  src.mono = a.mono;
  s.sources.push_back(src);
  if (output) {
    s.output.enabled = true;
    s.output.device = a.out;
    s.output.channel = a.channel;
    s.output.mono = a.mono;
  }
  return s;
}

// Callback sizes and timing of a capture stream (how bursty it is).
int probe_mode(const Args& a) {
  auto be = desk::make_system_backend();
  struct Rec : desk::AudioSink {
    std::vector<std::pair<int64_t, size_t>> calls;
    std::mutex mu;
    void audio(const float*, size_t frames, int64_t end_ns) override {
      std::lock_guard<std::mutex> l(mu);
      calls.push_back({end_ns, frames});
    }
  } rec;
  std::string err;
  vc::DesktopAudioSource src;
  parse_source(a.source, &src);
  std::unique_ptr<desk::Stream> st;
  const int64_t t0 = virgil::mono_ns();
  if (!be->connect(&err)) {
    std::printf("not available: %s\n", err.c_str());
    return 1;
  }
  std::printf("connected in %.1f ms\n", (virgil::mono_ns() - t0) / 1e6);
  if (src.type == vc::DesktopAudioSource::kDevice) {
    st = be->open_device_capture(src.device, 2, &rec, &err);
  } else {
    auto streams = be->app_streams(src.app, src.type == vc::DesktopAudioSource::kAllApps);
    if (!streams.empty()) st = be->open_app_capture(streams[0], 2, &rec, &err);
  }
  if (!st || !st->start(&err)) {
    std::printf("cannot capture: %s\n", err.c_str());
    return 1;
  }
  const int64_t t1 = virgil::mono_ns();
  std::printf("started in %.1f ms\n", (t1 - t0) / 1e6);
  wait_ms(int(a.seconds * 1000));
  st.reset();
  {
    std::lock_guard<std::mutex> l(rec.mu);
    if (!rec.calls.empty()) std::printf("first audio %.1f ms after start\n", (rec.calls[0].first - t1) / 1e6);
  }
  std::lock_guard<std::mutex> l(rec.mu);
  if (rec.calls.size() < 2) {
    std::printf("%zu callbacks\n", rec.calls.size());
    return 1;
  }
  const double span = (rec.calls.back().first - rec.calls.front().first) / 1e9;
  size_t frames = 0, maxf = 0;
  int64_t max_gap = 0;
  for (size_t i = 0; i < rec.calls.size(); ++i) {
    frames += rec.calls[i].second;
    maxf = std::max(maxf, rec.calls[i].second);
    if (i) max_gap = std::max(max_gap, rec.calls[i].first - rec.calls[i - 1].first);
  }
  std::printf("%zu callbacks in %.2f s, %.1f frames/s, largest %zu frames, longest gap %.1f ms, rate %u\n",
              rec.calls.size(), span, (frames - rec.calls.front().second) / span, maxf, max_gap / 1e6, st ? 0u : 0u);
  return 0;
}

int list_mode() {
  auto be = desk::make_system_backend();
  std::string why;
  if (!be->connect(&why)) {
    std::printf("not available: %s\n", why.c_str());
    return 1;
  }
  std::printf("default output: %s\n", be->default_output().c_str());
  for (const auto& d : be->outputs())
    std::printf("output %s%s  (%s, %u channels)\n", d.id.c_str(), d.is_default ? " *" : "", d.name.c_str(), d.channels);
  std::printf("default input: %s\n", be->default_input().c_str());
  for (const auto& d : be->inputs())
    std::printf("input %s%s  (%s, %u channels)\n", d.id.c_str(), d.is_default ? " *" : "", d.name.c_str(), d.channels);
  for (const auto& a : be->apps())
    std::printf("app %s  (%s)%s\n", a.id.c_str(), a.name.c_str(), a.playing ? " playing" : "");
  return 0;
}

// Reads the "Desktop audio" slot of a running virgild while the engine runs.
int tx_mode(const Args& a) {
  desk::Engine e(desk::make_system_backend(), a.shm);
  e.start(settings_for(a, false));
  virgil::Client c;
  for (int k = 0; k < 50 && !c.open(a.shm); ++k) wait_ms(100);
  if (!c.is_open()) {
    std::printf("cannot open %s: %s\n", a.shm.c_str(), c.open_error().c_str());
    return 1;
  }
  const unsigned C = c.tx_channels(), rate = c.sample_rate(), ring = c.ring_frames();
  auto* h = const_cast<virgil::ShmHeader*>(c.header());
  const int64_t t0 = virgil::mono_ns();
  int good = 0, windows = 0, slot = -1;
  double first_sound_s = -1;
  while (virgil::mono_ns() - t0 < int64_t(a.seconds * 1e9)) {
    wait_ms(250);
    print_status(e);
    if (slot < 0)
      for (unsigned i = 0; i < virgil::kMaxTxClients; ++i)
        if (h->clients[i].pid.load() && virgil::slot_name(h->clients[i]) == "Desktop audio") slot = int(i);
    if (slot < 0) continue;
    // The daemon consumes (and clears) the slot ring tick by tick: collect
    // frames as they are written, before their tick comes.
    const uint32_t n = 4096;
    const float* r = virgil::tx_ring(h, uint32_t(slot));
    std::vector<std::vector<float>> chans(C, std::vector<float>(n));
    uint32_t have = 0;
    uint64_t next = 0;
    for (int64_t until = virgil::mono_ns() + 1000000000LL; have < n && virgil::mono_ns() < until;) {
      const uint64_t now = h->now_frames.load();
      const uint64_t w = h->clients[slot].written_until.load();
      if (next < now) {  // missed some (or the first round): start over
        next = std::max(now, w > 400 ? w - 400 : 0);
        have = 0;
      }
      for (; next < w && have < n; ++next, ++have)
        for (unsigned ch = 0; ch < C; ++ch) chans[ch][have] = r[size_t(next & (ring - 1)) * C + ch];
      virgil::sleep_until_ns(virgil::mono_ns() + 200000);
    }
    if (have < n) {
      std::printf("      could not follow the slot ring\n");
      continue;
    }
    ++windows;
    bool ok = true;
    std::printf("      slot %d:", slot);
    for (unsigned ch = 0; ch < C; ++ch) {
      double rms, share;
      analyse(chans[ch], rate, a.freq, &rms, &share);
      const bool mapped = ch + 1 == a.channel || (!a.mono && ch == a.channel);
      std::printf(" ch%u %.3f/%.0f%%", ch + 1, rms, share * 100);
      if (mapped && !(rms > 0.05 && share > a.share)) ok = false;
      if (!mapped && rms > 1e-4) ok = false;
    }
    std::printf("%s\n", ok ? "  ok" : "");
    if (ok) {
      ++good;
      if (first_sound_s < 0) first_sound_s = (virgil::mono_ns() - t0) / 1e9;
    }
  }
  e.shutdown();
  std::printf("RESULT tx: %d of %d windows had the %.0f Hz tone on the mapped channels only (first after %.2f s)\n",
              good, windows, a.freq, first_sound_s);
  return good >= windows / 2 && good > 0 ? 0 : 1;
}

// Onsets of tone bursts on an output device, timed on mono_ns (via a capture
// of the device's monitor).
struct Onsets : desk::AudioSink {
  unsigned rate = 48000;
  std::mutex mu;
  std::vector<int64_t> at;
  int64_t quiet = 0;  // frames below the threshold
  void audio(const float* d, size_t frames, int64_t end_ns) override {
    std::lock_guard<std::mutex> l(mu);
    for (size_t f = 0; f < frames; ++f) {
      const float v = d ? std::fabs(d[2 * f]) : 0.f;
      if (v > 0.1f) {
        if (quiet > int64_t(rate / 20))
          at.push_back(end_ns - int64_t(double(frames - 1 - f) * 1e9 / rate));
        quiet = 0;
      } else {
        ++quiet;
      }
    }
  }
};

// A Dante clock running `ppm` fast against the system clock.
class SkewClock : public virgil::ClockSource {
 public:
  explicit SkewClock(double ppm) {
    virgil::FreeRunClock f;
    m_ = f.model();
    m_.ratio = 1 + ppm * 1e-6;
  }
  virgil::ClockModel model() const override { return m_; }
  virgil::DaemonState state() const override { return virgil::kStateFreeRun; }

 private:
  virgil::ClockModel m_;
};

int loop_mode(const Args& a) {
  virgil::g_log_level = virgil::kLogWarn;
  virgil::Config c;
  c.device_name = "DA Loop";
  c.interface = "127.0.0.1";
  c.shm_name = std::string(virgil::kDefaultShmName) + "-da-e2e-" + std::to_string(virgil::process_id());
  c.clock = "free";
  c.lock_memory = false;
  c.tx_channels = 4;
  c.rx_channels = 4;
  c.latency_us = 10000;
  c.tx_latency_us = 4000;
  c.ring_frames = 16384;
  virgil::Engine ve(c);
  ve.set_dante_enabled(false);
  if (!ve.start(std::unique_ptr<virgil::ClockSource>(new SkewClock(a.ppm)))) {
    std::printf("cannot start the Virgil engine\n");
    return 1;
  }
  wait_ms(50);
  const uint64_t L = c.us_to_frames(c.tx_send_delay_us()), RL = c.us_to_frames(c.latency_us);
  const uint64_t D = c.us_to_frames(c.tx_latency_us) + RL;
  const uint64_t txmask = c.dante_tx_ring_frames() - 1, rxmask = c.dante_rx_ring_frames() - 1;
  const unsigned C = 4;
  std::printf("Virgil loop: tx -> rx in %llu frames (%.1f ms) at %u Hz, clock %+.0f ppm\n", (unsigned long long)D,
              D * 1000.0 / c.sample_rate, c.sample_rate, a.ppm);
  std::atomic<bool> sim_run{true};
  std::atomic<float> tx_peak{0};
  std::thread sim([&] {
    auto& tx = ve.dante_tx_ring();
    auto& rx = ve.dante_rx_ring();
    uint64_t done = 0;
    while (sim_run) {
      wait_ms(1);
      const uint64_t media = ve.header()->now_frames.load();
      if (!media) continue;  // no tick yet: 0 is not a media time
      if (done == 0) done = media - 64;
      const uint64_t lag = std::min<uint64_t>(L, 2 * c.period_frames());
      const uint64_t sent_until = media > lag ? media - lag : 0;
      float pk = tx_peak.load();
      for (; done < sent_until; ++done)
        for (uint32_t ch = 0; ch < C; ++ch) {
          const int32_t v = tx[size_t(done & txmask) * C + ch];
          rx[size_t((done + D) & rxmask) * C + ch] = v;
          pk = std::max(pk, std::fabs(float(v) / 2147483648.f));
        }
      tx_peak.store(pk);
    }
  });
  desk::Engine e(desk::make_system_backend(), c.shm_name);
  e.start(settings_for(a, true));
  // Latency: the source device's sound against the output device's.
  std::unique_ptr<desk::Backend> mb;
  std::unique_ptr<desk::Stream> ms, md;
  Onsets src_on, dst_on;
  if (!a.measure_src.empty()) {
    mb = desk::make_system_backend();
    std::string err;
    ms = mb->open_device_capture(a.measure_src, 2, &src_on, &err);
    md = mb->open_device_capture(a.out, 2, &dst_on, &err);
    if (ms) src_on.rate = ms->rate();
    if (md) dst_on.rate = md->rate();
    if (!ms || !md || !ms->start(&err) || !md->start(&err)) std::printf("cannot measure: %s\n", err.c_str());
  }
  const int64_t t0 = virgil::mono_ns();
  while (virgil::mono_ns() - t0 < int64_t(a.seconds * 1e9)) {
    wait_ms(500);
    print_status(e);
    std::printf("      Virgil transmit peak %.3f\n", double(tx_peak.exchange(0)));
  }
  const desk::Status st = e.status();
  if (ms && md) {
    ms.reset();
    md.reset();
    std::vector<double> d;
    for (int64_t t : src_on.at) {
      // The first onset on the output within 1 s after it.
      for (int64_t u : dst_on.at)
        if (u > t && u - t < 1000000000LL) {
          d.push_back((u - t) / 1e6);
          break;
        }
    }
    std::sort(d.begin(), d.end());
    const double loop_ms = D * 1000.0 / c.sample_rate;
    if (d.empty()) {
      std::printf("RESULT latency: no bursts matched (%zu at the source, %zu at the output)\n", src_on.at.size(),
                  dst_on.at.size());
    } else {
      std::printf("RESULT latency over %zu bursts: min %.1f, median %.1f, max %.1f ms device to device; "
                  "minus Virgil's own %.1f ms loop: median %.1f ms for desktop audio (capture + output)\n",
                  d.size(), d.front(), d[d.size() / 2], d.back(), loop_ms, d[d.size() / 2] - loop_ms);
    }
  }
  e.shutdown();
  sim_run = false;
  sim.join();
  ve.stop();
  bool ok = !st.sources.empty() && !st.sources[0].error && !st.output.error;
  if (!a.measure_src.empty() && dst_on.at.empty()) ok = false;
  std::printf("RESULT loop: %s\n", ok ? "running" : "errors");
  return ok ? 0 : 1;
}


struct Stats {
  std::vector<double> v;
  void add(double x) { v.push_back(x); }
  std::string text() {
    if (v.empty()) return "none";
    std::sort(v.begin(), v.end());
    char b[128];
    std::snprintf(b, sizeof b, "min %.2f, median %.2f, max %.2f ms over %zu", v.front(), v[v.size() / 2], v.back(),
                  v.size());
    return b;
  }
};

// The test signal: channel k (1-based) is +-scale k, negative during the
// first tenth of every second (frame counts at `rate`).
float test_value(uint64_t frame, unsigned k, unsigned rate, float scale) {
  return (frame % rate < rate / 10 ? -scale : scale) * float(k);
}

// An audio interface's inputs (a pipe source this program feeds) -> a running virgild.
int inputs_mode(const Args& a) {
  if (a.fifo.empty() || !a.dev_ch) {
    std::printf("inputs needs --fifo and --dev-ch\n");
    return 2;
  }
  const unsigned D = a.dev_ch, rate = 48000;
  const int fd = open(a.fifo.c_str(), O_WRONLY);
  if (fd < 0) {
    std::printf("cannot open %s\n", a.fifo.c_str());
    return 1;
  }
  // The writer: 1 ms chunks in real time; when each second's flip was written.
  std::atomic<bool> run{true};
  std::mutex wmu;
  std::deque<int64_t> flips;  // mono_ns of each flip's write, oldest first
  std::thread writer([&] {
    const size_t n = rate / 1000;
    std::vector<float> buf(n * D);
    uint64_t frame = 0;
    int64_t next = virgil::mono_ns();
    while (run) {
      for (size_t f = 0; f < n; ++f)
        for (unsigned k = 0; k < D; ++k) buf[f * D + k] = test_value(frame + f, k + 1, rate, 0.05f);
      const int64_t now = virgil::mono_ns();
      if (frame % rate == 0) {
        std::lock_guard<std::mutex> l(wmu);
        flips.push_back(now);
        if (flips.size() > 64) flips.pop_front();
      }
      const ssize_t w = write(fd, buf.data(), buf.size() * sizeof(float));
      if (w < 0) break;
      frame += n;
      next += 1000000;
      virgil::sleep_until_ns(next);
    }
  });

  vc::DesktopAudioSettings s;
  s.enabled = true;
  vc::DesktopAudioSource src;
  if (!parse_source(a.source, &src) || src.type != vc::DesktopAudioSource::kInput) {
    std::printf("inputs needs --source input:<source>\n");
    return 2;
  }
  src.first_input = a.first;
  src.inputs = a.count;
  src.channel = a.channel;
  s.sources.push_back(src);
  desk::Engine e(desk::make_system_backend(), a.shm);
  e.start(s);
  virgil::Client c;
  for (int k = 0; k < 50 && !c.open(a.shm); ++k) wait_ms(100);
  if (!c.is_open()) {
    std::printf("cannot open %s: %s\n", a.shm.c_str(), c.open_error().c_str());
    run = false;
    writer.join();
    return 1;
  }
  const unsigned C = c.tx_channels(), ring = c.ring_frames();
  auto* h = const_cast<virgil::ShmHeader*>(c.header());
  const int64_t t0 = virgil::mono_ns();
  int slot = -1;
  uint64_t next = 0, checked = 0, exact = 0, bad_other = 0;
  int sign = 0;  // of the first mapped channel, with hysteresis (+1, -1)
  const float thr = 0.025f * float(a.first);
  int64_t settle_until = 0, next_print = 0;
  uint64_t skip_until = 0;  // frames near a flip (the resampler's kernel rings there)
  std::deque<uint64_t> recent_bad;
  Stats lat;
  std::vector<double> sum(C, 0.0);
  uint64_t nsum = 0;
  while (virgil::mono_ns() - t0 < int64_t(a.seconds * 1e9)) {
    const int64_t now_ns = virgil::mono_ns();
    if (now_ns >= next_print) {
      next_print = now_ns + 1000000000LL;
      print_status(e);
    }
    if (slot < 0) {
      for (unsigned i = 0; i < virgil::kMaxTxClients; ++i)
        if (h->clients[i].pid.load() && virgil::slot_name(h->clients[i]) == "Desktop audio") slot = int(i);
      wait_ms(5);
      continue;
    }
    const float* r = virgil::tx_ring(h, uint32_t(slot));
    const uint64_t now = h->now_frames.load();
    const uint64_t w = h->clients[slot].written_until.load();
    if (next < now) {  // missed some (or the first round): start over
      next = std::max(now, w > 400 ? w - 400 : 0);
      skip_until = next + 256;
    }
    virgil::ClockAnchor an;
    const bool have_anchor = c.anchor(&an);
    const bool counting = now_ns - t0 > 3000000000LL;  // bridges settled
    for (; next < w; ++next) {
      const float* fr = r + size_t(next & (ring - 1)) * C;
      const float v0 = fr[a.channel - 1];
      // A flip to negative: its Dante transmit time against when it was written.
      const int sg = v0 > thr ? 1 : v0 < -thr ? -1 : sign;
      if (sign > 0 && sg < 0 && have_anchor && counting && now_ns > settle_until) {
        const int64_t tx_ns = an.host_ns_at(double(next));
        std::lock_guard<std::mutex> l(wmu);
        for (auto it = flips.rbegin(); it != flips.rend(); ++it)
          if (*it < tx_ns) {
            if (tx_ns - *it < 900000000LL) lat.add((tx_ns - *it) / 1e6);
            break;
          }
      }
      if (sg != sign) {
        // The resampler's kernel rings on both sides of a step: the frames
        // just before it do not count either.
        skip_until = next + 64;
        while (!recent_bad.empty() && recent_bad.back() + 64 > next) {
          recent_bad.pop_back();
          --checked;
        }
      }
      sign = sg;
      if (!counting || next < skip_until) continue;
      ++checked;
      bool ok = true;
      for (unsigned ch = 0; ch < C; ++ch) {
        const bool mapped = ch + 1 >= a.channel && ch + 1 < a.channel + a.count;
        if (mapped) {
          const unsigned k = a.first + (ch + 1 - a.channel);
          const float want = (sign < 0 ? -0.05f : 0.05f) * float(k);
          if (std::fabs(fr[ch] - want) > 1e-4f) ok = false;
          sum[ch] += std::fabs(fr[ch]);
        } else if (std::fabs(fr[ch]) > 1e-6f) {
          ++bad_other;
          ok = false;
        }
      }
      ++nsum;
      if (ok) ++exact;
      else recent_bad.push_back(next);
      if (recent_bad.size() > 256) recent_bad.pop_front();
    }
    virgil::sleep_until_ns(virgil::mono_ns() + 200000);
  }
  e.shutdown();
  run = false;
  close(fd);
  writer.join();
  std::printf("      mean |value| per transmit channel:");
  for (unsigned ch = 0; ch < C; ++ch) std::printf(" %.4f", nsum ? sum[ch] / double(nsum) : 0.0);
  std::printf("\n");
  std::printf("RESULT inputs: %llu of %llu frames exact on tx %u-%u (inputs %u-%u), %llu with sound elsewhere; "
              "latency written -> Dante transmit time: %s (Virgil's transmit lead %.1f ms included)\n",
              (unsigned long long)exact, (unsigned long long)checked, a.channel, a.channel + a.count - 1, a.first,
              a.first + a.count - 1, (unsigned long long)bad_other, lat.text().c_str(),
              c.tx_lead_frames() * 1000.0 / c.sample_rate());
  return checked > 48000 && exact >= checked * 995 / 1000 && !lat.v.empty() ? 0 : 1;
}

// Dante receive channels -> an audio interface's outputs (a null sink, read back with parec).
int outputs_mode(const Args& a) {
  if (!a.dev_ch) {
    std::printf("outputs needs --dev-ch\n");
    return 2;
  }
  virgil::g_log_level = virgil::kLogWarn;
  virgil::Config c;
  c.device_name = "DA Out";
  c.interface = "127.0.0.1";
  c.shm_name = std::string(virgil::kDefaultShmName) + "-da-e2o-" + std::to_string(virgil::process_id());
  c.clock = "free";
  c.lock_memory = false;
  c.tx_channels = 2;
  c.rx_channels = 8;
  c.latency_us = 10000;
  c.tx_latency_us = 4000;
  c.ring_frames = 16384;
  virgil::Engine ve(c);
  ve.set_dante_enabled(false);
  if (!ve.start()) {
    std::printf("cannot start the Virgil engine\n");
    return 1;
  }
  const unsigned R = c.rx_channels, rate = c.sample_rate;
  const uint64_t dmask = c.dante_rx_ring_frames() - 1;
  std::atomic<bool> sim_run{true};
  std::thread sim([&] {
    auto& rx = ve.dante_rx_ring();
    uint64_t done = 0;
    while (sim_run) {
      wait_ms(1);
      const uint64_t media = ve.header()->now_frames.load();
      if (!media) continue;
      if (done + c.ring_frames / 2 < media) done = media;
      for (; done < media + 8000; ++done)
        for (uint32_t ch = 0; ch < R; ++ch)
          rx[size_t(done & dmask) * R + ch] = int32_t(std::lround(double(test_value(done, ch + 1, rate, 0.1f)) * 2147483647.0));
    }
  });
  virgil::Client vc_;
  for (int k = 0; k < 20 && !vc_.open(c.shm_name); ++k) wait_ms(50);

  vc::DesktopAudioSettings s;
  s.enabled = true;
  s.output.enabled = true;
  s.output.type = vc::DesktopAudioOutput::kInterface;
  s.output.device = a.out;
  s.output.channel = a.channel;
  s.output.first_output = a.first;
  s.output.outputs = a.count;
  desk::Engine e(desk::make_system_backend(), c.shm_name);
  e.start(s);

  // parec of the sink's monitor, every channel, by index.
  const unsigned D = a.dev_ch;
  const std::string cmd = "exec parec -d '" + a.out + ".monitor' --raw --format=float32le --rate=" +
                          std::to_string(rate) + " --channels=" + std::to_string(D) +
                          " --no-remap --no-remix --latency-msec=2 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) {
    std::printf("cannot run parec\n");
    return 1;
  }
  std::atomic<bool> run{true};
  std::mutex mu;
  uint64_t checked = 0, exact = 0, bad_other = 0;
  std::vector<double> sum(D, 0.0);
  Stats lat;
  const int64_t t0 = virgil::mono_ns();
  std::thread reader([&] {
    std::vector<float> buf(4096 * D);
    int sign = 0;  // of the first mapped output, with hysteresis
    const float thr = 0.05f * float(a.channel);
    uint64_t skip = 0, total = 0;
    std::deque<uint64_t> recent_bad;  // inexact frames just before a flip do not count
    size_t carry = 0;  // bytes of a partial frame at the start of buf
    for (;;) {
      const ssize_t n = read(fileno(p), reinterpret_cast<char*>(buf.data()) + carry, buf.size() * 4 - carry);
      if (n <= 0) break;
      const int64_t at = virgil::mono_ns();
      const size_t bytes = carry + size_t(n), frames = bytes / (4 * D);
      const bool counting = at - t0 > 3000000000LL;
      virgil::ClockAnchor an;
      const bool have = vc_.is_open() && vc_.anchor(&an);
      std::lock_guard<std::mutex> l(mu);
      for (size_t f = 0; f < frames; ++f, ++total) {
        const float* fr = &buf[f * D];
        const float v0 = fr[a.first - 1];
        const int sg = v0 > thr ? 1 : v0 < -thr ? -1 : sign;
        if (sign > 0 && sg < 0 && have && counting) {
          // Arrived about now (minus the frames after it in this read); due at
          // Dante when its media frame (a multiple of the rate) was.
          const int64_t arr = at - int64_t(double(frames - 1 - f) * 1e9 / rate);
          const double fnow = an.frame_at(arr);
          for (uint64_t m = uint64_t(fnow) / rate * rate; m + rate > uint64_t(fnow); m -= rate) {
            const int64_t due = an.host_ns_at(double(m));
            if (due < arr) {
              lat.add((arr - due) / 1e6);
              break;
            }
            if (m < rate) break;
          }
        }
        if (sg != sign) {
          skip = total + 64;
          while (!recent_bad.empty() && recent_bad.back() + 64 > total) {
            recent_bad.pop_back();
            --checked;
          }
        }
        sign = sg;
        if (!counting || total < skip) continue;
        ++checked;
        bool ok = true;
        for (unsigned k = 0; k < D; ++k) {
          const bool mapped = k + 1 >= a.first && k + 1 < a.first + a.count;
          if (mapped) {
            const unsigned rxk = a.channel + (k + 1 - a.first);
            const float want = (sign < 0 ? -0.1f : 0.1f) * float(rxk);
            if (std::fabs(fr[k] - want) > 1e-4f) ok = false;
          } else if (std::fabs(fr[k]) > 1e-6f) {
            ++bad_other;
            ok = false;
          }
          sum[k] += std::fabs(fr[k]);
        }
        if (ok) ++exact;
        else recent_bad.push_back(total);
        if (!ok && std::getenv("E2E_TRACE")) {
          static int shown = 0;
          if (shown++ < 200) std::printf("TRACE bad %llu (flip+%lld): %.5f %.5f %.5f %.5f\n", (unsigned long long)total, (long long)(total - skip + 64), fr[a.first-1], fr[a.first], fr[a.first+1], fr[a.first+2]);
        }
        if (recent_bad.size() > 256) recent_bad.pop_front();
      }
      carry = bytes - frames * 4 * D;
      if (carry) std::memmove(buf.data(), reinterpret_cast<char*>(buf.data()) + frames * 4 * D, carry);
      if (!run) break;
    }
  });
  while (virgil::mono_ns() - t0 < int64_t(a.seconds * 1e9)) {
    wait_ms(1000);
    print_status(e);
  }
  const desk::Status st = e.status();
  run = false;
  e.shutdown();
  wait_ms(200);
  // parec never ends on its own.
  if (FILE* k = popen(("pkill -f -- \"-d '" + a.out + ".monitor' --raw\" ").c_str(), "r")) pclose(k);
  reader.join();
  pclose(p);
  sim_run = false;
  sim.join();
  ve.stop();
  std::lock_guard<std::mutex> l(mu);
  std::printf("      mean |value| per output:");
  for (unsigned k = 0; k < D; ++k) std::printf(" %.4f", checked ? sum[k] / double(checked) : 0.0);
  std::printf("\n");
  std::printf("RESULT outputs: %llu of %llu frames exact on outputs %u-%u (rx %u-%u), %llu with sound elsewhere; "
              "latency Dante media time -> device: %s (Virgil's receive latency %.1f ms included)\n",
              (unsigned long long)exact, (unsigned long long)checked, a.first, a.first + a.count - 1, a.channel,
              a.channel + a.count - 1, (unsigned long long)bad_other, lat.text().c_str(), c.latency_us / 1000.0);
  return !st.output.error && checked > 48000 && exact >= checked * 995 / 1000 && !lat.v.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s list|probe|tx|loop|inputs|outputs [options] (see the source)\n", argv[0]);
    return 2;
  }
  a.mode = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string k = argv[i];
    auto val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (k == "--shm") a.shm = val();
    else if (k == "--source") a.source = val();
    else if (k == "--out") a.out = val();
    else if (k == "--channel") a.channel = unsigned(std::atoi(val().c_str()));
    else if (k == "--mono") a.mono = true;
    else if (k == "--seconds") a.seconds = std::atof(val().c_str());
    else if (k == "--freq") a.freq = std::atof(val().c_str());
    else if (k == "--ppm") a.ppm = std::atof(val().c_str());
    else if (k == "--measure") a.measure_src = val();
    else if (k == "--share") a.share = std::atof(val().c_str());
    else if (k == "--fifo") a.fifo = val();
    else if (k == "--first") a.first = unsigned(std::atoi(val().c_str()));
    else if (k == "--count") a.count = unsigned(std::atoi(val().c_str()));
    else if (k == "--dev-ch") a.dev_ch = unsigned(std::atoi(val().c_str()));
    else {
      std::fprintf(stderr, "unknown option %s\n", k.c_str());
      return 2;
    }
  }
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  if (a.mode == "list") return list_mode();
  if (a.mode == "probe") return probe_mode(a);
  if (a.mode == "tx") return tx_mode(a);
  if (a.mode == "loop") return loop_mode(a);
  if (a.mode == "inputs") return inputs_mode(a);
  if (a.mode == "outputs") return outputs_mode(a);
  std::fprintf(stderr, "unknown mode %s\n", a.mode.c_str());
  return 2;
}
