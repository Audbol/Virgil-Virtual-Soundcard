#include "desktop_audio_engine.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#if !defined(_WIN32)
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

#include "virgil/client.h"
#include "virgil/platform.h"
#include "virgil/resampler.h"

namespace desk {

namespace {

constexpr unsigned kMaxLanes = 32;          // capture streams in one session
constexpr unsigned kMaxStreamsPerApp = 16;  // streams of one app (or of "all apps")
constexpr int64_t kStaleNs = 2000000000LL;  // virgild silent this long: gone
constexpr int64_t kSoundHoldNs = 500000000LL;
constexpr float kSilence = 1e-5f;  // -100 dBFS
constexpr unsigned kMaxWidth = vc::kMaxInterfaceInputs;  // channels of one lane (interface inputs)

std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

void peak_max(std::atomic<uint32_t>& a, float v) {
  uint32_t want;
  std::memcpy(&want, &v, 4);
  uint32_t cur = a.load(std::memory_order_relaxed);
  for (;;) {
    float f;
    std::memcpy(&f, &cur, 4);
    if (!(v > f)) return;
    if (a.compare_exchange_weak(cur, want, std::memory_order_relaxed)) return;
  }
}
float peak_take(std::atomic<uint32_t>& a) {
  const uint32_t b = a.exchange(0, std::memory_order_relaxed);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

std::string rate_text(unsigned rate) {
  return rate % 1000 ? fmt("%.1f kHz", rate / 1000.0) : fmt("%u kHz", rate / 1000);
}

// Bridge level for an audio interface's inputs / outputs: a few ms. The FIFO
// swings by one device period on one side and one I/O block (block_s) on the
// other; twice the period, the block and 2 ms of scheduling jitter keep it
// from running dry. period_s: the stream's, 0 = unknown (5 ms assumed).
double interface_target_s(double period_s, double block_s) {
  const double p = period_s > 0 ? std::min(period_s, 0.050) : 0.005;
  return std::max(0.006, 2 * p + block_s + 0.002);
}

// The entry for `want` ("default" / "" = the flagged default) in a device list.
bool find_device(const std::vector<Device>& list, const std::string& want, Device* out) {
  const bool def = want.empty() || want == "default";
  for (const Device& d : list)
    if (def ? d.is_default : d.id == want) {
      *out = d;
      return true;
    }
  return false;
}

std::string plural(unsigned n, const char* one, const char* many) { return fmt("%u %s", n, n == 1 ? one : many); }

// VIRGIL_DA_DEBUG=1 in the environment: the bridges' level and counters in
// the status texts (for tuning and bug reports).
std::string bridge_detail(const virgil::AdaptiveBridge& b) {
  static const bool on = std::getenv("VIRGIL_DA_DEBUG") != nullptr;
  if (!on) return std::string();
  return fmt(" [level %.1f ms, %llu underruns, %llu overflows]", b.level_ms(), (unsigned long long)b.underruns(),
             (unsigned long long)b.overflows());
}

// One capture stream's way to the I/O thread. The backend's audio thread is
// the bridge's producer, the I/O thread its consumer.
struct Lane : AudioSink {
  std::mutex mu;                  // the I/O thread try-locks it per block
  bool live = false;              // guarded by mu
  unsigned source = 0, first = 0, width = 2;  // guarded by mu; width up to kMaxWidth
  virgil::AdaptiveBridge bridge;  // configured under mu by the owner; then SPSC
  std::atomic<bool> claimed{false};

  void audio(const float* data, size_t frames, int64_t end_ns) override { bridge.push(data, frames, end_ns); }
};

// Per source (and the output), shared between threads.
struct Shared {
  std::atomic<float> gain{1.f};
  std::atomic<uint32_t> peak{0};
  std::atomic<int64_t> sound_ns{0};  // last block above silence
  // Guarded by Session::st_mu_.
  std::string text = "Starting…";
  bool error = false;
  std::string endpoint;  // resolved output device (feedback check)
};

class Session : AudioSource {
 public:
  Session(virgil::Client& c, const vc::DesktopAudioSettings& cfg, Backend& be)
      : c_(c), cfg_(cfg), be_(be), src_(new Shared[std::max<size_t>(1, cfg.sources.size())]) {
    rate_ = c.sample_rate();
    txch_ = c.tx_channels();
    rxch_ = c.rx_channels();
    ring_ = c.ring_frames();
    tx_lead_ = c.tx_lead_frames();
    rx_lat_ = c.rx_latency_frames();
    // ~5 ms blocks on the media clock (240 frames at 48 kHz).
    B_ = std::max(16u, (rate_ / 200 + 15) / 16 * 16);
    while (B_ > 16 && B_ >= ring_ / 8) B_ /= 2;
    mix_.assign(size_t(B_) * std::max(1u, txch_), 0.f);
    tmp_.assign(size_t(B_) * kMaxWidth, 0.f);
    rxbuf_.assign(size_t(B_) * kMaxWidth, 0.f);
    set_gains(cfg);
  }
  ~Session() override { stop(); }

  void start() {
    io_ = std::thread([this] { io_loop(); });
    for (unsigned i = 0; i < cfg_.sources.size(); ++i) workers_.emplace_back([this, i] { source_loop(i); });
    if (cfg_.output.enabled) workers_.emplace_back([this] { output_loop(); });
  }
  void stop() {
    {
      std::lock_guard<std::mutex> l(quit_mu_);
      quit_.store(true, std::memory_order_release);
    }
    quit_cv_.notify_all();
    for (auto& t : workers_)
      if (t.joinable()) t.join();
    workers_.clear();
    if (io_.joinable()) io_.join();
  }
  bool lost() const { return lost_.load(std::memory_order_acquire); }

  void set_gains(const vc::DesktopAudioSettings& s) {
    for (size_t i = 0; i < s.sources.size() && i < cfg_.sources.size(); ++i) {
      const auto& src = s.sources[i];
      src_[i].gain.store(src.mute ? 0.f : float(vc::db_to_gain(src.gain_db)), std::memory_order_relaxed);
    }
    out_.gain.store(float(vc::db_to_gain(s.output.gain_db)), std::memory_order_relaxed);
  }

  // Into the engine's status (called with the engine lock held).
  void collect(Status* st) {
    const int64_t now = virgil::mono_ns();
    std::lock_guard<std::mutex> l(st_mu_);
    st->sources.resize(cfg_.sources.size());
    std::vector<std::string> captured;  // device sources' resolved devices
    for (size_t i = 0; i < cfg_.sources.size(); ++i) {
      SourceStatus& s = st->sources[i];
      s.text = src_[i].text;
      s.error = src_[i].error;
      s.peak = peak_take(src_[i].peak);
      s.sound = now - src_[i].sound_ns.load(std::memory_order_relaxed) < kSoundHoldNs;
      if (cfg_.sources[i].type == vc::DesktopAudioSource::kDevice && !src_[i].endpoint.empty())
        captured.push_back(src_[i].endpoint);
    }
    if (cfg_.output.enabled) {
      st->output.text = out_.text;
      st->output.error = out_.error;
      st->output.peak = peak_take(out_.peak);
      st->output.sound = now - out_.sound_ns.load(std::memory_order_relaxed) < kSoundHoldNs;
      if (!out_.endpoint.empty() && std::find(captured.begin(), captured.end(), out_.endpoint) != captured.end()) {
        st->output.text =
            "Plays on a device that is also captured: it would echo back to Dante. Choose another device, or "
            "capture “all apps” instead.";
        st->output.error = true;
      }
    }
  }

 private:
  bool quitting() const { return quit_.load(std::memory_order_acquire); }
  // Sleeps up to ms; true if the session is ending.
  bool wait_quit(int ms) {
    std::unique_lock<std::mutex> l(quit_mu_);
    quit_cv_.wait_for(l, std::chrono::milliseconds(ms), [this] { return quitting(); });
    return quitting();
  }

  void set_text(Shared& s, const std::string& t, bool error) {
    std::lock_guard<std::mutex> l(st_mu_);
    s.text = t;
    s.error = error;
  }
  void set_endpoint(Shared& s, const std::string& e) {
    std::lock_guard<std::mutex> l(st_mu_);
    s.endpoint = e;
  }

  // A free lane for a stream of `source`; configured once its rate is known.
  int claim_lane(unsigned source, unsigned first, unsigned width) {
    for (unsigned k = 0; k < kMaxLanes; ++k) {
      Lane& ln = lanes_[k];
      bool expected = false;
      if (!ln.claimed.compare_exchange_strong(expected, true)) continue;
      std::lock_guard<std::mutex> l(ln.mu);
      ln.live = false;
      ln.source = source;
      ln.first = first;
      ln.width = width;
      return int(k);
    }
    return -1;
  }
  // Before the stream starts (no producer yet).
  bool arm_lane(int k, unsigned in_rate, double target_s, double slack_s) {
    Lane& ln = lanes_[k];
    virgil::BridgeConfig bc;
    bc.channels = ln.width;
    bc.in_rate = in_rate;
    bc.out_rate = rate_;
    bc.target_s = target_s;
    bc.slack_s = slack_s;
    bc.max_pull = B_;
    std::lock_guard<std::mutex> l(ln.mu);
    if (!ln.bridge.init(bc)) return false;
    ln.live = true;
    return true;
  }
  // After its stream is destroyed (no producer any more).
  void release_lane(int k) {
    if (k < 0) return;
    Lane& ln = lanes_[k];
    {
      std::lock_guard<std::mutex> l(ln.mu);
      ln.live = false;
    }
    ln.claimed.store(false, std::memory_order_release);
  }

  // ---- I/O thread: Virgil's media clock (timing as in the drivers) -------------------
  void io_loop() {
    raise_audio_thread_priority(int64_t(B_) * 1000000000LL / std::max(1u, rate_), 50);
    const uint64_t B = B_;
    virgil::Client::AnchorCache cache;
    uint64_t steps = c_.clock_steps();
    double now = 0;
    c_.frame_now(&now, steps, &cache);
    uint64_t t = (uint64_t(now) / B + 1) * B;
    const uint64_t max_ahead = 2 * B + rate_ / 20, max_behind = ring_ / 4;
    const int64_t max_sleep_ns = 20000000;
    while (!quitting()) {
      // virgild stopped ticking (stopped, restarted, new layout): the
      // supervisor tears this session down and reconnects.
      if (!c_.heartbeat_fresh(kStaleNs)) {
        lost_.store(true, std::memory_order_release);
        if (wait_quit(50)) break;
        continue;
      }
      const uint64_t cur_steps = c_.clock_steps();
      virgil::ClockAnchor a;
      if (!c_.anchor(&a, cur_steps, &cache)) {
        virgil::sleep_until_ns(virgil::mono_ns() + 1000000);
        continue;
      }
      const int64_t now_ns = virgil::mono_ns();
      const double now_frame = a.frame_at(now_ns);
      if (cur_steps != steps || double(t) > now_frame + double(max_ahead)) {
        steps = cur_steps;  // new timeline: start at the next boundary
        t = (uint64_t(now_frame) / B + 1) * B;
      } else if (double(t) + double(max_behind) < now_frame) {
        t = (uint64_t(now_frame) / B + 1) * B;  // stalled: skip what was missed
      }
      const int64_t deadline = a.host_ns_at(double(t));
      if (deadline - now_ns > max_sleep_ns) {
        virgil::sleep_until_ns(now_ns + max_sleep_ns);
        continue;
      }
      virgil::sleep_until_ns(deadline);
      if (quitting()) break;
      block(t, deadline);
      t += B;
    }
    if (c_.has_tx_slot() && tx_active_) c_.set_tx_active(false);
  }

  void block(uint64_t t, int64_t due) {
    const uint32_t B = B_, txch = txch_;
    if (c_.has_tx_slot() && txch) {
      std::fill(mix_.begin(), mix_.end(), 0.f);
      bool sound = false;
      for (Lane& ln : lanes_) {
        if (!ln.claimed.load(std::memory_order_acquire)) continue;
        std::unique_lock<std::mutex> l(ln.mu, std::try_to_lock);
        if (!l.owns_lock() || !ln.live) continue;
        if (!ln.bridge.pull(tmp_.data(), B, due)) continue;  // priming / nothing playing
        Shared& s = src_[ln.source];
        const float g = s.gain.load(std::memory_order_relaxed);
        const unsigned w = ln.width;
        float pk = 0;
        for (uint32_t f = 0; f < B; ++f)
          for (unsigned k = 0; k < w; ++k) {
            const float v = g * tmp_[size_t(f) * w + k];
            pk = std::max(pk, std::fabs(v));
            const unsigned ch = ln.first + k;
            if (ch < txch) mix_[size_t(f) * txch + ch] += v;
          }
        peak_max(s.peak, pk);
        if (pk > kSilence) {
          s.sound_ns.store(due, std::memory_order_relaxed);
          sound = true;
        }
      }
      // "Playing" in Virgil's app list only while there is sound; quiet for
      // 2 s and the daemon just consumes the (silent) ring.
      if (sound) {
        last_sound_ns_ = due;
        if (!tx_active_) {
          c_.set_tx_active(true);
          tx_active_ = true;
        }
      } else if (tx_active_ && due - last_sound_ns_ > 2000000000LL) {
        c_.set_tx_active(false);
        tx_active_ = false;
      }
      if (tx_active_) c_.write_tx(t + B + tx_lead_, mix_.data(), B, txch);
      c_.touch();
    }
    if (cfg_.output.enabled && rxch_) {
      std::unique_lock<std::mutex> l(out_mu_, std::try_to_lock);
      // Only once the device asks for audio (it may take a moment to start):
      // a bridge nobody drains just overflows.
      if (l.owns_lock() && out_live_ && out_pulling_.load(std::memory_order_acquire) && t > uint64_t(B) + rx_lat_) {
        // The block that just became readable (as the drivers read it).
        const uint64_t start = t - B - rx_lat_;
        const unsigned c0 = cfg_.output.channel - 1, c1 = cfg_.output.mono ? c0 : c0 + 1;
        const float g = out_.gain.load(std::memory_order_relaxed);
        float pk = 0;
        if (cfg_.output.type == vc::DesktopAudioOutput::kInterface) {
          // Receive channels channel.. -> the interface's outputs, one each.
          const unsigned w = std::min(kMaxWidth, cfg_.output.width());
          for (uint32_t f = 0; f < B; ++f) {
            const float* fr = c_.rx_frame(start + f);
            for (unsigned k = 0; k < w; ++k) {
              const float v = c0 + k < rxch_ ? g * fr[c0 + k] : 0.f;
              rxbuf_[size_t(f) * w + k] = v;
              pk = std::max(pk, std::fabs(v));
            }
          }
        } else {
          for (uint32_t f = 0; f < B; ++f) {
            const float* fr = c_.rx_frame(start + f);
            const float lv = c0 < rxch_ ? g * fr[c0] : 0.f, rv = c1 < rxch_ ? g * fr[c1] : 0.f;
            rxbuf_[2 * f] = lv;
            rxbuf_[2 * f + 1] = rv;
            pk = std::max(pk, std::max(std::fabs(lv), std::fabs(rv)));
          }
        }
        peak_max(out_.peak, pk);
        if (pk > kSilence) out_.sound_ns.store(due, std::memory_order_relaxed);
        out_bridge_.push(rxbuf_.data(), B, due);
      }
    }
  }

  // ---- source threads ------------------------------------------------------------------
  struct Capture {
    std::unique_ptr<Stream> st;
    int lane = -1;
    uint64_t id = 0;  // AppStream::id
  };

  // Opens a capture into a fresh lane and starts it; false with *err.
  template <class Open>
  bool open_capture(unsigned i, const vc::DesktopAudioSource& src, Capture* cap, std::string* err, Open&& open) {
    cap->lane = claim_lane(i, src.channel - 1, src.width());
    if (cap->lane < 0) {
      *err = "Too many capture streams";
      return false;
    }
    cap->st = open(&lanes_[cap->lane]);
    // Interface inputs: a few ms (steady callbacks of a known period). The
    // rest: capture callbacks come every 10-20 ms, sometimes in bursts.
    const bool low = src.type == vc::DesktopAudioSource::kInput;
    const double target = low ? interface_target_s(cap->st ? cap->st->period_s() : 0, double(B_) / rate_) : 0.050;
    if (cap->st && !arm_lane(cap->lane, cap->st->rate(), target, low ? 0.040 : 0.200)) {
      *err = "Out of memory";
      cap->st.reset();
    }
    if (cap->st && !cap->st->start(err)) cap->st.reset();
    if (!cap->st) {
      if (err->empty()) *err = "Cannot capture";
      release_lane(cap->lane);
      cap->lane = -1;
      return false;
    }
    return true;
  }
  void close_capture(Capture* cap) {
    cap->st.reset();  // no more callbacks into the lane after this
    release_lane(cap->lane);
    cap->lane = -1;
  }

  std::string lane_text(const Capture& c) const {
    const std::string note = c.st->note();
    if (!note.empty()) return note;
    const virgil::AdaptiveBridge& b = lanes_[c.lane].bridge;
    const std::string rate = rate_text(c.st->rate());
    if (b.state() != virgil::AdaptiveBridge::kRunning) return "Waiting for sound (" + rate + ")";
    return fmt("Capturing (%s, clock %+.0f ppm)", rate.c_str(), b.adjust_ppm()) + bridge_detail(b);
  }

  void source_loop(unsigned i) {
    raise_audio_thread_priority(10000000, 20);
    const vc::DesktopAudioSource& src = cfg_.sources[i];
    Shared& sh = src_[i];
    // Interface inputs: every one needs a channel (the others may run off the end, as before).
    const unsigned last = src.type == vc::DesktopAudioSource::kInput ? src.channel + src.width() - 1 : src.channel;
    if (src.channel < 1 || last > txch_) {
      set_text(sh, fmt("Virgil has %u transmit channels: choose others", txch_), true);
      return;
    }
    if (src.type == vc::DesktopAudioSource::kDevice) device_source(i, src);
    else if (src.type == vc::DesktopAudioSource::kInput) input_source(i, src);
    else app_source(i, src);
  }

  // An audio interface's inputs first_input.. -> transmit channels channel.., one each.
  void input_source(unsigned i, const vc::DesktopAudioSource& src) {
    Shared& sh = src_[i];
    const bool follows_default = src.device.empty() || src.device == "default";
    const unsigned count = src.width();
    auto wait_change = [&](unsigned changes0) {  // again in 3 s, or as soon as a device comes or goes
      for (int k = 0; k < 30 && !quitting() && be_.changes() == changes0; ++k) wait_quit(100);
    };
    while (!quitting()) {
      const unsigned changes0 = be_.changes();
      unsigned defaults = be_.default_changes();
      Device dev;
      if (!find_device(be_.inputs(), src.device, &dev)) {
        set_text(sh, follows_default ? "There is no input device" : "The device is not connected", true);
        wait_change(changes0);
        continue;
      }
      if (src.first_input < 1 || (dev.channels && src.first_input - 1 + count > dev.channels)) {
        set_text(sh, "The device has " + plural(dev.channels, "input", "inputs") + ": choose others", true);
        wait_change(changes0);
        continue;
      }
      Capture cap;
      std::string err;
      if (!open_capture(i, src, &cap, &err, [&](Lane* ln) {
            return be_.open_input_capture(dev.id, src.first_input, count, ln, &err);
          })) {
        set_text(sh, err, true);
        wait_change(changes0);
        continue;
      }
      int64_t next_text = 0;
      std::string why;
      bool failed = false;
      while (!wait_quit(50)) {
        if ((failed = cap.st->failed(&why))) break;
        if (follows_default && be_.default_changes() != defaults) {
          defaults = be_.default_changes();
          const std::string now_default = be_.default_input();
          if (!now_default.empty() && now_default != cap.st->device()) break;  // follow the new default
        }
        const int64_t now = virgil::mono_ns();
        if (now >= next_text) {
          next_text = now + 250000000;
          const virgil::AdaptiveBridge& b = lanes_[cap.lane].bridge;
          const std::string note = cap.st->note();
          const std::string rate = rate_text(cap.st->rate());
          set_text(sh,
                   !note.empty() ? note
                   : b.frames_in() == 0 ? "No audio from the device yet (" + rate + ")"
                   : b.state() != virgil::AdaptiveBridge::kRunning
                       ? "Starting (" + rate + ")"
                       : fmt("Capturing %s (%s, clock %+.0f ppm)", plural(count, "input", "inputs").c_str(),
                             rate.c_str(), b.adjust_ppm()) +
                             bridge_detail(b),
                   false);
        }
      }
      close_capture(&cap);
      if (failed) {
        set_text(sh, (why.empty() ? std::string("The device stopped") : why) + ": retrying", true);
        wait_quit(500);
      }
    }
  }

  void device_source(unsigned i, const vc::DesktopAudioSource& src) {
    Shared& sh = src_[i];
    const bool follows_default = src.device.empty() || src.device == "default";
    const unsigned width = src.width();
    while (!quitting()) {
      const unsigned changes0 = be_.changes();
      unsigned defaults = be_.default_changes();
      Capture cap;
      std::string err;
      if (!open_capture(i, src, &cap, &err, [&](Lane* ln) {
            return be_.open_device_capture(src.device, width, ln, &err);
          })) {
        set_text(sh, err, true);
        // Again in 3 s, or as soon as a device comes or goes.
        for (int k = 0; k < 30 && !quitting() && be_.changes() == changes0; ++k) wait_quit(100);
        continue;
      }
      set_endpoint(sh, cap.st->device());
      int64_t next_text = 0;
      std::string why;
      bool failed = false;
      while (!wait_quit(50)) {
        if ((failed = cap.st->failed(&why))) break;
        if (follows_default && be_.default_changes() != defaults) {
          defaults = be_.default_changes();
          const std::string now_default = be_.default_output();
          if (!now_default.empty() && now_default != cap.st->device()) break;  // follow the new default
        }
        const int64_t now = virgil::mono_ns();
        if (now >= next_text) {
          next_text = now + 250000000;
          const virgil::AdaptiveBridge& b = lanes_[cap.lane].bridge;
          const std::string note = cap.st->note();
          set_text(sh,
                   !note.empty() ? note
                   : b.frames_in() == 0 || b.state() != virgil::AdaptiveBridge::kRunning
                       ? "Nothing playing (" + rate_text(cap.st->rate()) + ")"
                       : lane_text(cap),
                   false);
        }
      }
      close_capture(&cap);
      if (failed) {
        set_text(sh, (why.empty() ? std::string("The device stopped") : why) + ": retrying", true);
        wait_quit(500);
      }
    }
  }

  void app_source(unsigned i, const vc::DesktopAudioSource& src) {
    Shared& sh = src_[i];
    const bool all = src.type == vc::DesktopAudioSource::kAllApps;
    const Capabilities caps = be_.capabilities();
    if (!(all ? caps.all_apps : caps.app_capture)) {
      set_text(sh, caps.app_hint.empty() ? std::string("Not available on this computer") : caps.app_hint, true);
      while (!wait_quit(1000)) {}
      return;
    }
    if (!all && src.app.empty()) {
      set_text(sh, "No app chosen", true);
      while (!wait_quit(1000)) {}
      return;
    }
    std::vector<Capture> caps_open;
    std::map<uint64_t, int64_t> retry_at;  // stream id -> not before
    std::string last_error;
    int64_t next_resolve = 0, next_text = 0;
    unsigned seen_changes = be_.changes() - 1;
    while (!quitting()) {
      const int64_t now = virgil::mono_ns();
      if (now >= next_resolve || be_.changes() != seen_changes) {
        // Which streams to capture (again on every change, and every 2 s:
        // apps open and close streams as they play and pause).
        seen_changes = be_.changes();
        next_resolve = now + 2000000000LL;
        std::vector<AppStream> want = be_.app_streams(src.app, all);
        if (want.size() > kMaxStreamsPerApp) want.resize(kMaxStreamsPerApp);
        for (auto it = caps_open.begin(); it != caps_open.end();) {
          if (std::none_of(want.begin(), want.end(), [&](const AppStream& w) { return w.id == it->id; })) {
            close_capture(&*it);
            it = caps_open.erase(it);
          } else {
            ++it;
          }
        }
        for (const AppStream& w : want) {
          if (quitting()) break;
          if (std::any_of(caps_open.begin(), caps_open.end(), [&](const Capture& c) { return c.id == w.id; }))
            continue;
          auto r = retry_at.find(w.id);
          if (r != retry_at.end() && now < r->second) continue;
          Capture cap;
          cap.id = w.id;
          std::string err;
          if (!open_capture(i, src, &cap, &err, [&](Lane* ln) {
                return be_.open_app_capture(w, src.width(), ln, &err);
              })) {
            retry_at[w.id] = now + 5000000000LL;
            last_error = err;
            continue;
          }
          retry_at.erase(w.id);
          caps_open.push_back(std::move(cap));
        }
        for (auto it = retry_at.begin(); it != retry_at.end();)  // forget streams long gone
          it = std::none_of(want.begin(), want.end(), [&](const AppStream& w) { return w.id == it->first; })
                   ? retry_at.erase(it)
                   : std::next(it);
        next_text = 0;
      }
      for (auto it = caps_open.begin(); it != caps_open.end();) {
        std::string why;
        if (it->st->failed(&why)) {
          // E.g. the app closed the stream; the next resolve opens it again if it is back.
          retry_at[it->id] = virgil::mono_ns() + 1000000000LL;
          close_capture(&*it);
          it = caps_open.erase(it);
          next_resolve = 0;
        } else {
          ++it;
        }
      }
      if (now >= next_text) {
        next_text = now + 250000000;
        if (caps_open.empty()) {
          if (!last_error.empty() && !retry_at.empty()) set_text(sh, last_error, true);
          else set_text(sh, all ? "Nothing playing" : "Not playing right now", false);
        } else {
          // The busiest stream speaks for the source.
          const Capture* best = &caps_open[0];
          for (const Capture& c : caps_open)
            if (lanes_[c.lane].bridge.state() == virgil::AdaptiveBridge::kRunning) best = &c;
          std::string t = lane_text(*best);
          if (caps_open.size() > 1) t += fmt(", %u streams", unsigned(caps_open.size()));
          set_text(sh, t, false);
          last_error.clear();
        }
      }
      wait_quit(50);
    }
    for (Capture& c : caps_open) close_capture(&c);
  }

  // ---- output thread: receive channels -> an output device ---------------------------
  void render(float* out, size_t frames, int64_t now_ns) override {
    // Backend's audio thread, the bridge's consumer: what the device has
    // played since the last call is what it asks for now, so the time of
    // the call is the consumption time.
    out_bridge_.pull(out, frames, now_ns);
    if (!out_pulling_.load(std::memory_order_relaxed)) out_pulling_.store(true, std::memory_order_release);
  }

  void output_loop() {
    raise_audio_thread_priority(10000000, 20);
    const vc::DesktopAudioOutput& o = cfg_.output;
    const bool iface = o.type == vc::DesktopAudioOutput::kInterface;
    const unsigned width = iface ? std::min(kMaxWidth, o.width()) : 2;
    const unsigned last = iface ? o.channel + width - 1 : o.channel;
    if (o.channel < 1 || last > rxch_) {
      set_text(out_,
               rxch_ ? fmt("Virgil has %u receive channels: choose others", rxch_)
                     : std::string("Virgil has no receive channels (Settings)"),
               true);
      return;
    }
    while (!quitting()) {
      const unsigned changes0 = be_.changes();
      unsigned defaults = be_.default_changes();
      std::string err;
      std::unique_ptr<Stream> r;
      Device dev;
      if (iface) {
        // An interface's outputs first_output.. (the device's other outputs get silence).
        if (!find_device(be_.outputs(), o.device, &dev)) {
          err = o.device.empty() || o.device == "default" ? "There is no output device" : "The device is not connected";
        } else if (o.first_output < 1 || (dev.channels && o.first_output - 1 + width > dev.channels)) {
          err = "The device has " + plural(dev.channels, "output", "outputs") + ": choose others";
        } else {
          r = be_.open_render(dev.id, o.first_output, width, 0.020, this, &err);
        }
      } else {
        r = be_.open_render(o.device, 0, 2, 0.030, this, &err);
      }
      if (r) {
        virgil::BridgeConfig bc;
        bc.channels = width;
        bc.in_rate = rate_;
        bc.out_rate = r->rate();
        bc.target_s = iface ? interface_target_s(r->period_s(), double(B_) / rate_) : 0.040;
        bc.slack_s = iface ? 0.040 : 0.200;
        bc.max_pull = 2048;  // larger requests are split
        std::lock_guard<std::mutex> l(out_mu_);
        out_pulling_.store(false, std::memory_order_relaxed);
        out_live_ = out_bridge_.init(bc);
        if (!out_live_) {
          err = "Out of memory";
          r.reset();
        }
      }
      if (r) {
        set_endpoint(out_, r->device());
        if (!r->start(&err)) r.reset();
      }
      if (!r) {
        {
          std::lock_guard<std::mutex> l(out_mu_);
          out_live_ = false;
        }
        set_text(out_, err.empty() ? std::string("Cannot play") : err, true);
        for (int k = 0; k < 30 && !quitting() && be_.changes() == changes0; ++k) wait_quit(100);
        continue;
      }
      int64_t next_text = 0;
      std::string why;
      bool failed = false;
      const std::string dev_name = !dev.name.empty() ? dev.name : !o.name.empty() ? o.name : r->device();
      while (!wait_quit(50)) {
        if ((failed = r->failed(&why))) break;
        if (o.device.empty() || o.device == "default") {
          if (be_.default_changes() != defaults) {
            defaults = be_.default_changes();
            const std::string now_default = be_.default_output();
            if (!now_default.empty() && now_default != r->device()) break;
          }
        }
        const int64_t now = virgil::mono_ns();
        if (now >= next_text) {
          next_text = now + 250000000;
          const std::string note = r->note();
          const bool run = out_bridge_.state() == virgil::AdaptiveBridge::kRunning;
          const std::string rate = rate_text(r->rate());
          set_text(out_,
                   !note.empty() ? note
                   : !run        ? std::string("Starting…")
                   : iface       ? fmt("Playing %s on %s (%s, clock %+.0f ppm)",
                                       plural(width, "channel", "channels").c_str(), dev_name.c_str(), rate.c_str(),
                                       out_bridge_.adjust_ppm()) +
                                 bridge_detail(out_bridge_)
                                 : fmt("Playing (%s, clock %+.0f ppm)", rate.c_str(), out_bridge_.adjust_ppm()) +
                                 bridge_detail(out_bridge_),
                   false);
        }
      }
      r.reset();  // no more render calls after this
      {
        std::lock_guard<std::mutex> l(out_mu_);
        out_live_ = false;
      }
      if (failed) {
        set_text(out_, (why.empty() ? std::string("The device stopped") : why) + ": retrying", true);
        wait_quit(500);
      }
    }
  }

  virgil::Client& c_;
  const vc::DesktopAudioSettings cfg_;
  Backend& be_;
  uint32_t rate_ = 48000, txch_ = 0, rxch_ = 0, ring_ = 0, tx_lead_ = 0, rx_lat_ = 0, B_ = 240;
  Lane lanes_[kMaxLanes];
  std::unique_ptr<Shared[]> src_;
  Shared out_;
  std::mutex out_mu_;  // the I/O thread try-locks it per block
  bool out_live_ = false;
  std::atomic<bool> out_pulling_{false};  // the render stream has asked for audio
  virgil::AdaptiveBridge out_bridge_;
  std::atomic<bool> quit_{false}, lost_{false};
  std::mutex quit_mu_;
  std::condition_variable quit_cv_;
  std::mutex st_mu_;
  std::thread io_;
  std::vector<std::thread> workers_;
  // I/O thread only.
  std::vector<float> mix_, tmp_, rxbuf_;
  bool tx_active_ = false;
  int64_t last_sound_ns_ = 0;
};

}  // namespace

// ---- scheduling ---------------------------------------------------------------------------

void raise_audio_thread_priority(int64_t period_ns, int rt_priority) {
#if defined(_WIN32) || defined(__APPLE__)
  // Windows: MMCSS "Pro Audio"; macOS: the time-constraint policy.
  virgil::set_realtime_priority(rt_priority, period_ns);
#else
  // SCHED_FIFO where RLIMIT_RTPRIO allows it (audio group, PipeWire's
  // limits.d), else a lower nice value (RLIMIT_NICE), else as it is.
  if (virgil::set_realtime_priority(std::max(1, std::min(99, rt_priority)), period_ns)) return;
#if defined(__linux__)
  setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), -11);
#endif
#endif
}

// ---- Engine ---------------------------------------------------------------------------------

struct Engine::Impl {
  std::unique_ptr<Backend> be;
  std::string shm_name;
  std::mutex mu;  // guards everything below except the thread
  std::condition_variable wake_cv;
  bool wake = false;
  vc::DesktopAudioSettings want, applied;
  bool changed = false, started = false;
  Status st;
  std::unique_ptr<Session> session;
  std::atomic<bool> quit{false};
  std::thread th;

  void set_phase(Status::Phase p, const std::string& text) {
    std::lock_guard<std::mutex> l(mu);
    st.phase = p;
    st.text = text;
  }

  void publish_layout(virgil::Client& c, bool connected) {
    std::lock_guard<std::mutex> l(mu);
    st.connected = connected;
    if (!connected) return;
    st.rate = c.sample_rate();
    st.tx = c.tx_channels();
    st.rx = c.rx_channels();
    st.tx_names.assign(st.tx, std::string());
    st.rx_names.assign(st.rx, std::string());
    for (unsigned i = 0; i < st.tx; ++i) st.tx_names[i] = c.channel_name(true, i);
    for (unsigned i = 0; i < st.rx; ++i) st.rx_names[i] = c.channel_name(false, i);
  }

  void teardown(virgil::Client& c) {
    std::unique_ptr<Session> s;
    {
      std::lock_guard<std::mutex> l(mu);
      s = std::move(session);
    }
    if (s) s->stop();  // joins its threads (never under the lock)
    s.reset();
    if (c.is_open() && c.has_tx_slot()) {
      c.set_tx_active(false);
      c.release_tx_slot();
    }
  }

  void sleep(int ms) {
    std::unique_lock<std::mutex> l(mu);
    wake_cv.wait_for(l, std::chrono::milliseconds(ms), [this] { return wake || quit.load(); });
    wake = false;
  }

  void run() {
    virgil::Client client;
    vc::DesktopAudioSettings cur;
    int64_t retry_at = 0;
    uint32_t names_seq = ~0u;
    while (!quit.load(std::memory_order_acquire)) {
      sleep(250);
      if (quit.load(std::memory_order_acquire)) break;
      bool fresh = false;
      vc::DesktopAudioSettings next;
      {
        std::lock_guard<std::mutex> l(mu);
        if (changed) {
          next = want;
          changed = false;
          fresh = true;
        }
      }
      if (fresh) {
        if (session && next.same_routes(cur)) {
          session->set_gains(next);  // gain / mute: no restart
        } else {
          teardown(client);
          retry_at = 0;
        }
        cur = next;
        std::lock_guard<std::mutex> l(mu);
        applied = cur;
      }
      const bool needed = cur.enabled && (!cur.sources.empty() || cur.output.enabled);
      if (!needed) {
        teardown(client);
        client.close();
        publish_layout(client, false);
        set_phase(Status::kOff, cur.enabled ? "Add a source to send this computer’s audio to Dante."
                                            : "Desktop audio is off.");
        continue;
      }
      const int64_t now = virgil::mono_ns();
      if (session) {
        if (session->lost() || !client.heartbeat_fresh(kStaleNs)) {
          teardown(client);
          client.close();
          publish_layout(client, false);
          set_phase(Status::kWaiting, "Virgil stopped or restarted: reconnecting…");
          retry_at = now + 500000000LL;
          continue;
        }
        if (client.names_seq() != names_seq) {
          names_seq = client.names_seq();
          publish_layout(client, true);
        }
        set_phase(Status::kRunning, fmt("On: %u source%s to Virgil%s (%s).", unsigned(cur.sources.size()),
                                        cur.sources.size() == 1 ? "" : "s",
                                        cur.output.enabled ? ", receive channels to an output device" : "",
                                        rate_text(client.sample_rate()).c_str()));
        continue;
      }
      if (now < retry_at) continue;
      retry_at = now + 1000000000LL;
      // The sound system first: without it there is nothing to do with
      // Virgil's playback slot.
      std::string why;
      if (!be->connect(&why)) {
        publish_layout(client, false);
        set_phase(Status::kError, why);
        retry_at = now + 3000000000LL;
        continue;
      }
      if (!client.is_open() && !client.open(shm_name)) {
        publish_layout(client, false);
        set_phase(Status::kWaiting, "Waiting for Virgil: " + client.open_error());
        continue;
      }
      if (!client.daemon_alive(500000000LL)) {
        const bool stopped = client.header()->state.load(std::memory_order_acquire) == virgil::kStateStopped;
        set_phase(Status::kWaiting, stopped ? "Virgil is stopped. Desktop audio starts again with Virgil."
                                            : "Waiting for Virgil: the service is not responding.");
        // Closed so the next try finds a new soundcard (restart, new layout).
        if (!client.heartbeat_fresh(kStaleNs) || stopped) client.close();
        publish_layout(client, false);
        continue;
      }
      names_seq = client.names_seq();
      publish_layout(client, true);
      if (!cur.sources.empty() && !client.acquire_tx_slot("Desktop audio")) {
        set_phase(Status::kError, fmt("All %u Virgil playback slots are in use: close an app that plays to Virgil.",
                                      virgil::kMaxTxClients));
        client.close();
        retry_at = now + 5000000000LL;
        continue;
      }
      if (!cur.sources.empty()) client.clear_pending_tx();
      std::unique_ptr<Session> s(new Session(client, cur, *be));
      s->start();
      {
        std::lock_guard<std::mutex> l(mu);
        session = std::move(s);
      }
      set_phase(Status::kRunning, "Starting…");
    }
    teardown(client);
    client.close();
  }

  void poke() {
    {
      std::lock_guard<std::mutex> l(mu);
      wake = true;
    }
    wake_cv.notify_all();
  }
};

Engine::Engine(std::unique_ptr<Backend> backend, std::string shm_name) : impl_(new Impl) {
  impl_->be = std::move(backend);
  impl_->shm_name = std::move(shm_name);
}
Engine::~Engine() { shutdown(); }

Backend& Engine::backend() { return *impl_->be; }

void Engine::start(const vc::DesktopAudioSettings& s) {
  {
    std::lock_guard<std::mutex> l(impl_->mu);
    if (impl_->started) return;
    impl_->started = true;
    impl_->want = s;
    impl_->applied = s;
    impl_->changed = true;
  }
  impl_->th = std::thread([this] { impl_->run(); });
  impl_->poke();
}

void Engine::apply(const vc::DesktopAudioSettings& s) {
  {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->want = s;
    impl_->changed = true;
  }
  impl_->poke();
}

void Engine::shutdown() {
  if (!impl_) return;
  impl_->quit.store(true, std::memory_order_release);
  impl_->poke();
  if (impl_->th.joinable()) impl_->th.join();
}

vc::DesktopAudioSettings Engine::settings() {
  std::lock_guard<std::mutex> l(impl_->mu);
  return impl_->changed ? impl_->want : impl_->applied;
}

Status Engine::status() {
  const Capabilities caps = impl_->be->capabilities();
  std::lock_guard<std::mutex> l(impl_->mu);
  Status s = impl_->st;
  s.caps = caps;
  if (impl_->changed) {
    // New settings not picked up by the supervisor yet (a fraction of a
    // second): the running session's sources may not match them.
    s.sources.assign(impl_->want.sources.size(), SourceStatus());
    for (auto& src : s.sources) src.text = "Starting…";
    s.output.text = "Starting…";
  } else if (impl_->session) {
    impl_->session->collect(&s);
  } else {
    // Not running: every source just waits.
    const vc::DesktopAudioSettings& cfg = impl_->applied;
    const bool err = s.phase == Status::kError;
    const std::string wait = err ? std::string("Not running") : std::string("Waiting for Virgil");
    s.sources.assign(cfg.sources.size(), SourceStatus());
    for (auto& src : s.sources) src.text = cfg.enabled ? wait : "Off";
    s.output.text = cfg.output.enabled && cfg.enabled ? wait : "Off";
  }
  return s;
}

// ---- settings file --------------------------------------------------------------------------

std::string settings_path() {
  const char* home = std::getenv("HOME");
  const std::string h = home && *home ? home : "/tmp";
#if defined(__APPLE__)
  return h + "/Library/Application Support/Virgil/desktop-audio.conf";
#elif defined(_WIN32)
  const char* ad = std::getenv("APPDATA");
  return std::string(ad && *ad ? ad : ".") + "\\Virgil\\desktop-audio.conf";
#else
  const char* x = std::getenv("XDG_CONFIG_HOME");
  const std::string base = x && *x == '/' ? std::string(x) : h + "/.config";
  return base + "/virgil/desktop-audio.conf";
#endif
}

vc::DesktopAudioSettings load_settings(const std::string& path, std::string* error) {
  vc::DesktopAudioSettings s;
  std::ifstream f(path, std::ios::binary);
  if (!f) {
#if !defined(_WIN32)
    struct stat sb;
    const bool missing = stat(path.c_str(), &sb) != 0 && errno == ENOENT;
#else
    const bool missing = true;
#endif
    if (error && !missing) *error = "cannot read " + path;
    return s;
  }
  std::string text;
  char buf[4096];
  while (text.size() < (1u << 20)) {
    f.read(buf, sizeof buf);
    if (f.gcount() <= 0) break;
    text.append(buf, size_t(f.gcount()));
  }
  std::vector<std::string> warnings;
  vc::parse_desktop_audio(text, &s, &warnings);
  if (error && !warnings.empty()) *error = warnings.front();
  return s;
}

bool save_settings(const std::string& path, const vc::DesktopAudioSettings& s, std::string* error) {
  const size_t slash = path.find_last_of("/\\");
#if !defined(_WIN32)
  if (slash != std::string::npos && slash > 0) {
    // mkdir -p
    const std::string dir = path.substr(0, slash);
    for (size_t p = 1; p <= dir.size(); ++p)
      if (p == dir.size() || dir[p] == '/') mkdir(dir.substr(0, p).c_str(), 0755);
  }
#else
  (void)slash;
#endif
  const std::string tmp = path + ".tmp";
  const std::string text = vc::format_desktop_audio(s);
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      if (error) *error = "cannot write " + tmp;
      return false;
    }
    f.write(text.data(), std::streamsize(text.size()));
    f.flush();
    if (!f) {
      f.close();
      std::remove(tmp.c_str());
      if (error) *error = "cannot write " + tmp;
      return false;
    }
  }
#if defined(_WIN32)
  std::remove(path.c_str());  // rename does not replace on Windows
#endif
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    if (error) *error = "cannot save " + path;
    return false;
  }
  return true;
}

}  // namespace desk
