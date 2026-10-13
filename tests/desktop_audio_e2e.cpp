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
//   SPEC: device:<sink>|device:default|app:<name>|all
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include "desktop_audio_engine.h"
#include "virgil/client.h"
#include "virgil/engine.h"
#include "virgil/log.h"
#include "virgil/media_clock.h"
#include "virgil/platform.h"

namespace {

void wait_ms(int ms) { virgil::sleep_until_ns(virgil::mono_ns() + int64_t(ms) * 1000000); }

struct Args {
  std::string mode, shm, source = "device:default", out = "default", measure_src;
  unsigned channel = 1;
  bool mono = false;
  double seconds = 4, freq = 1000, ppm = 0, share = 0.95;
};

bool parse_source(const std::string& spec, vc::DesktopAudioSource* s) {
  if (spec == "all") {
    s->type = vc::DesktopAudioSource::kAllApps;
  } else if (spec.rfind("device:", 0) == 0) {
    s->type = vc::DesktopAudioSource::kDevice;
    s->device = spec.substr(7);
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
  for (const auto& d : be->outputs()) std::printf("output %s%s  (%s)\n", d.id.c_str(), d.is_default ? " *" : "", d.name.c_str());
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
  const uint64_t dmask = c.ring_frames - 1;
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
      if (done == 0) done = media - 64;
      const uint64_t lag = std::min<uint64_t>(L, 2 * c.period_frames());
      const uint64_t sent_until = media > lag ? media - lag : 0;
      float pk = tx_peak.load();
      for (; done < sent_until; ++done)
        for (uint32_t ch = 0; ch < C; ++ch) {
          const int32_t v = tx[size_t(done & dmask) * C + ch];
          rx[size_t((done + D) & dmask) * C + ch] = v;
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

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s list|tx|loop [options] (see the source)\n", argv[0]);
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
  std::fprintf(stderr, "unknown mode %s\n", a.mode.c_str());
  return 2;
}
