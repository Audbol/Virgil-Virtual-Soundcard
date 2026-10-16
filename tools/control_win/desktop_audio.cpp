#include "desktop_audio.h"

#include "asio_convert.h"
#include "asio_host.h"
#include "wasapi.h"

#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "virgil/client.h"
#include "virgil/platform.h"
#include "virgil/resampler.h"

namespace da {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}
std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

namespace {

constexpr unsigned kMaxLanes = 16;            // capture streams in one session
constexpr unsigned kMaxStreamsPerApp = 8;     // process trees of one app
constexpr int64_t kStaleNs = 2000000000LL;    // virgild silent this long: gone
constexpr int64_t kSoundHoldNs = 500000000LL;
constexpr float kSilence = 1e-5f;             // -100 dBFS

std::wstring fmt(const wchar_t* f, ...) {
  wchar_t buf[512];
  va_list ap;
  va_start(ap, f);
  _vsnwprintf(buf, 511, f, ap);
  va_end(ap);
  buf[511] = 0;
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

bool is_asio_input(const vc::DesktopAudioSource& s) {
  return s.type == vc::DesktopAudioSource::kInput && s.api != "wasapi";
}
bool is_asio_output(const vc::DesktopAudioOutput& o) {
  return o.enabled && o.type == vc::DesktopAudioOutput::kInterface && o.api != "wasapi";
}

std::wstring rate_text(unsigned rate) {
  return rate % 1000 ? fmt(L"%.1f kHz", rate / 1000.0) : fmt(L"%u kHz", rate / 1000);
}

// One capture stream's way to the I/O thread.
struct Lane {
  std::mutex mu;                  // the I/O thread try-locks it per block
  bool live = false;              // guarded by mu
  unsigned source = 0, first = 0, width = 2;  // guarded by mu
  virgil::AdaptiveBridge bridge;  // configured under mu by the owner; then SPSC
  std::atomic<bool> claimed{false};
};

// Per source (and the output), shared between threads.
struct Shared {
  std::atomic<float> gain{1.f};
  std::atomic<uint32_t> peak{0};
  std::atomic<int64_t> sound_ns{0};  // last block above silence
  // Guarded by Session::st_mu_.
  std::wstring text = L"Starting\u2026";
  bool error = false;
  std::wstring endpoint;  // resolved output device (feedback check)
};

class Session {
 public:
  Session(virgil::Client& c, const vc::DesktopAudioSettings& cfg, wa::DeviceWatcher& watch)
      : c_(c), cfg_(cfg), watch_(watch), src_(new Shared[std::max<size_t>(1, cfg.sources.size())]) {
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
    tmp_.assign(size_t(B_) * std::max(2u, vc::kMaxInterfaceInputs), 0.f);  // a lane is up to 64 wide (interface inputs)
    rxbuf_.assign(size_t(B_) * std::max(2u, cfg.output.width()), 0.f);
    set_output_map(cfg.output.width());
    set_gains(cfg);
    quit_ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  ~Session() {
    stop();
    if (quit_ev_) CloseHandle(quit_ev_);
  }

  void start() {
    io_ = std::thread([this] { io_loop(); });
    bool asio = is_asio_output(cfg_.output);
    for (unsigned i = 0; i < cfg_.sources.size(); ++i) {
      if (is_asio_input(cfg_.sources[i])) asio = true;  // the ASIO thread serves it
      else workers_.emplace_back([this, i] { source_loop(i); });
    }
    if (cfg_.output.enabled && !is_asio_output(cfg_.output)) workers_.emplace_back([this] { output_loop(); });
    if (asio) workers_.emplace_back([this] { asio_loop(); });
  }
  void stop() {
    quit_.store(true, std::memory_order_release);
    if (quit_ev_) SetEvent(quit_ev_);
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
    std::vector<std::wstring> captured;  // device sources' resolved devices
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
        st->output.text = L"Plays on a device that is also captured: it would echo back to Dante. Choose another "
                          L"device, or capture \u201Call apps\u201D instead.";
        st->output.error = true;
      }
    }
  }

 private:
  bool quitting() const { return quit_.load(std::memory_order_acquire); }
  // Sleeps up to ms; true if the session is ending.
  bool wait_quit(DWORD ms) { return WaitForSingleObject(quit_ev_, ms) == WAIT_OBJECT_0 || quitting(); }

  void set_text(Shared& s, const std::wstring& t, bool error) {
    std::lock_guard<std::mutex> l(st_mu_);
    s.text = t;
    s.error = error;
  }
  void set_endpoint(Shared& s, const std::wstring& e) {
    std::lock_guard<std::mutex> l(st_mu_);
    s.endpoint = e;
  }

  // target_s: the FIFO level to keep (loopback packets come every 10 ms,
  // sometimes in bursts; an ASIO buffer every few ms).
  int claim_lane(unsigned source, unsigned first, unsigned width, double in_rate, double target_s = 0.050,
                 double slack_s = 0.200, double extrapolate_s = 0.030) {
    for (unsigned k = 0; k < kMaxLanes; ++k) {
      Lane& ln = lanes_[k];
      bool expected = false;
      if (!ln.claimed.compare_exchange_strong(expected, true)) continue;
      virgil::BridgeConfig bc;
      bc.channels = width;
      bc.in_rate = in_rate;
      bc.out_rate = rate_;
      bc.target_s = target_s;
      bc.slack_s = slack_s;
      bc.max_extrapolate_s = extrapolate_s;
      bc.max_pull = B_;
      std::lock_guard<std::mutex> l(ln.mu);
      if (!ln.bridge.init(bc)) {
        ln.claimed.store(false);
        return -1;
      }
      ln.source = source;
      ln.first = first;
      ln.width = width;
      ln.live = true;
      return int(k);
    }
    return -1;
  }
  // Receive channels the output carries (under out_mu_): kDevice left / right
  // (the same one if mono), kInterface channel.. one per output.
  void set_output_map(unsigned width) {
    const vc::DesktopAudioOutput& o = cfg_.output;
    out_map_.assign(std::max(1u, width), ~0u);
    const unsigned c0 = o.channel - 1;
    if (o.type == vc::DesktopAudioOutput::kInterface) {
      for (unsigned k = 0; k < width; ++k) out_map_[k] = c0 + k;
    } else {
      out_map_.assign(2, c0);
      if (!o.mono) out_map_[1] = c0 + 1;
    }
  }

  void release_lane(int k) {
    if (k < 0) return;
    Lane& ln = lanes_[k];
    {
      std::lock_guard<std::mutex> l(ln.mu);
      ln.live = false;
    }
    ln.claimed.store(false, std::memory_order_release);
  }

  // ---- I/O thread: Virgil's media clock (timing as in the ASIO driver) ---------------
  void io_loop() {
    virgil::set_realtime_priority(0, int64_t(B_) * 1000000000LL / std::max(1u, rate_));
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
      if (l.owns_lock() && out_live_ && t > uint64_t(B) + rx_lat_) {
        // The block that just became readable (as the ASIO driver reads it).
        const uint64_t start = t - B - rx_lat_;
        const unsigned w = unsigned(out_map_.size());
        const float g = out_.gain.load(std::memory_order_relaxed);
        float pk = 0;
        for (uint32_t f = 0; f < B; ++f) {
          const float* fr = c_.rx_frame(start + f);
          for (unsigned k = 0; k < w; ++k) {
            const unsigned ch = out_map_[k];
            const float v = ch < rxch_ ? g * fr[ch] : 0.f;
            rxbuf_[size_t(f) * w + k] = v;
            pk = std::max(pk, std::fabs(v));
          }
        }
        peak_max(out_.peak, pk);
        if (pk > kSilence) out_.sound_ns.store(due, std::memory_order_relaxed);
        out_bridge_.push(rxbuf_.data(), B, due);
      }
    }
  }

  // ---- capture threads ----------------------------------------------------------------
  struct Stream {
    std::unique_ptr<wa::Capture> cap;
    int lane = -1;
    DWORD pid = 0;
    bool failed = false;
    HRESULT hr = S_OK;
  };

  // Waits for packets (or 10 ms: loopback of an idle device signals
  // nothing), then moves everything pending into the streams' bridges.
  void pump(std::vector<Stream>& streams, unsigned width) {
    HANDLE h[MAXIMUM_WAIT_OBJECTS];
    DWORD n = 0;
    h[n++] = quit_ev_;
    for (auto& s : streams)
      if (s.cap->event() && n < MAXIMUM_WAIT_OBJECTS) h[n++] = s.cap->event();
    WaitForMultipleObjects(n, h, FALSE, 10);
    if (quitting()) return;
    for (auto& s : streams) {
      if (s.failed) continue;
      virgil::AdaptiveBridge& b = lanes_[s.lane].bridge;
      s.hr = s.cap->drain(width, [&](const wa::Capture::Packet& p) { b.push(p.data, p.frames, p.end_ns); });
      if (FAILED(s.hr)) s.failed = true;
    }
  }

  std::wstring lane_text(const Stream& s) const {
    const virgil::AdaptiveBridge& b = lanes_[s.lane].bridge;
    const std::wstring rate = rate_text(s.cap->format().rate);
    if (b.state() != virgil::AdaptiveBridge::kRunning) return L"Waiting for sound (" + rate + L")";
    return fmt(L"Capturing (%ls, clock %+.0f ppm)", rate.c_str(), b.adjust_ppm());
  }

  void source_loop(unsigned i) {
    wa::ComScope com;
    virgil::set_realtime_priority(0, 10000000);
    const vc::DesktopAudioSource& src = cfg_.sources[i];
    Shared& sh = src_[i];
    if (FAILED(com.hr)) {
      set_text(sh, L"Cannot start: COM failed", true);
      return;
    }
    if (src.channel < 1 || src.channel > txch_) {
      set_text(sh, fmt(L"Virgil has %u transmit channels: choose others", txch_), true);
      return;
    }
    if (src.type == vc::DesktopAudioSource::kDevice) device_source(i, src);
    else if (src.type == vc::DesktopAudioSource::kInput) input_source(i, src);
    else process_source(i, src);
  }

  void device_source(unsigned i, const vc::DesktopAudioSource& src) {
    Shared& sh = src_[i];
    const std::wstring id = widen(src.device);
    const bool follows_default = id.empty() || id == L"default";
    const unsigned width = src.width();
    while (!quitting()) {
      const unsigned changes0 = watch_.changes(), defaults0 = watch_.default_changes();
      std::vector<Stream> streams(1);
      Stream& s = streams[0];
      s.cap.reset(new wa::Capture());
      std::wstring err;
      HRESULT hr = s.cap->open_device(id, &err);
      if (SUCCEEDED(hr)) {
        set_endpoint(sh, s.cap->endpoint());
        s.lane = claim_lane(i, src.channel - 1, width, s.cap->format().rate);
        if (s.lane < 0) {
          hr = E_OUTOFMEMORY;
          err = L"too many capture streams";
        } else if (FAILED(hr = s.cap->start())) {
          err = L"cannot start capturing: " + wa::hr_text(hr);
        }
      }
      if (FAILED(hr)) {
        release_lane(s.lane);
        s.cap->close();
        set_text(sh, err, true);
        // Again in 3 s, or as soon as a device comes or goes.
        for (int k = 0; k < 30 && !quitting() && watch_.changes() == changes0; ++k) wait_quit(100);
        continue;
      }
      int64_t next_text = 0;
      unsigned defaults = defaults0;
      while (!quitting()) {
        pump(streams, width);
        if (s.failed) break;
        if (follows_default && watch_.default_changes() != defaults) {
          defaults = watch_.default_changes();
          if (wa::default_render_id() != s.cap->endpoint()) break;  // follow Windows' new default
        }
        const int64_t now = virgil::mono_ns();
        if (now >= next_text) {
          next_text = now + 250000000;
          const virgil::AdaptiveBridge& b = lanes_[s.lane].bridge;
          set_text(sh, b.frames_in() == 0 || b.state() != virgil::AdaptiveBridge::kRunning
                           ? L"Nothing playing (" + rate_text(s.cap->format().rate) + L")"
                           : lane_text(s),
                   false);
        }
      }
      release_lane(s.lane);
      s.cap->close();
      if (s.failed) {
        set_text(sh, wa::hr_text(s.hr) + L": retrying", true);
        wait_quit(500);
      }
    }
  }

  // Inputs past Virgil's last transmit channel (a note for the status).
  std::wstring tx_overflow(const vc::DesktopAudioSource& src, unsigned n) const {
    if (src.channel - 1 + n <= txch_) return std::wstring();
    return fmt(L"; Virgil has %u transmit channels: inputs past it are dropped", txch_);
  }

  // An audio interface's inputs through WASAPI (shared mode, the recording
  // device's own channels as they are). The fallback for ASIO.
  void input_source(unsigned i, const vc::DesktopAudioSource& src) {
    Shared& sh = src_[i];
    const std::wstring id = widen(src.device);
    const bool follows_default = id.empty() || id == L"default";
    while (!quitting()) {
      const unsigned changes0 = watch_.changes(), defaults0 = watch_.capture_default_changes();
      std::vector<Stream> streams(1);
      Stream& s = streams[0];
      s.cap.reset(new wa::Capture());
      std::wstring err;
      HRESULT hr = s.cap->open_input(id, &err);
      asioconv::Range r;
      if (SUCCEEDED(hr)) {
        r = asioconv::select_channels(src.first_input, src.inputs, s.cap->format().channels);
        if (r.count == 0) {
          hr = E_INVALIDARG;
          err = fmt(L"the device has %u input%ls: choose others", s.cap->format().channels,
                    s.cap->format().channels == 1 ? L"" : L"s");
        }
      }
      if (SUCCEEDED(hr)) {
        s.cap->select_channels(r.first0, r.count);
        // Packets every device period (10 ms in shared mode).
        s.lane = claim_lane(i, src.channel - 1, r.count, s.cap->format().rate, double(B_) / rate_ + 0.025, 0.100, 0.030);
        if (s.lane < 0) {
          hr = E_OUTOFMEMORY;
          err = L"too many capture streams";
        } else if (FAILED(hr = s.cap->start())) {
          err = L"cannot start recording: " + wa::hr_text(hr);
        }
      }
      if (FAILED(hr)) {
        release_lane(s.lane);
        s.cap->close();
        set_text(sh, err, true);
        for (int k = 0; k < 30 && !quitting() && watch_.changes() == changes0; ++k) wait_quit(100);
        continue;
      }
      std::wstring extra = tx_overflow(src, r.count);
      if (r.clamped) extra += fmt(L"; the device has %u inputs", s.cap->format().channels);
      int64_t next_text = 0;
      unsigned defaults = defaults0;
      while (!quitting()) {
        pump(streams, r.count);
        if (s.failed) break;
        if (follows_default && watch_.capture_default_changes() != defaults) {
          defaults = watch_.capture_default_changes();
          if (wa::default_capture_id() != s.cap->endpoint()) break;  // follow Windows' new default
        }
        const int64_t now = virgil::mono_ns();
        if (now >= next_text) {
          next_text = now + 250000000;
          const virgil::AdaptiveBridge& b = lanes_[s.lane].bridge;
          const std::wstring rate = rate_text(s.cap->format().rate);
          set_text(sh, b.state() != virgil::AdaptiveBridge::kRunning
                           ? L"Starting (" + rate + L", WASAPI)" + extra
                           : fmt(L"Capturing %u input%ls (%ls, WASAPI, clock %+.0f ppm)", r.count, r.count == 1 ? L"" : L"s",
                                 rate.c_str(), b.adjust_ppm()) + extra,
                   false);
        }
      }
      release_lane(s.lane);
      s.cap->close();
      if (s.failed) {
        set_text(sh, wa::hr_text(s.hr) + L": retrying", true);
        wait_quit(500);
      }
    }
  }

  void process_source(unsigned i, const vc::DesktopAudioSource& src) {
    Shared& sh = src_[i];
    if (!wa::process_loopback_supported()) {
      set_text(sh, std::wstring(L"App capture ") + wa::kNeedsWin2004, true);
      while (!wait_quit(1000)) {}
      return;
    }
    const bool all = src.type == vc::DesktopAudioSource::kAllApps;
    const std::wstring exe = widen(src.app);
    const unsigned width = src.width();
    std::vector<Stream> streams;
    std::map<DWORD, int64_t> retry_at;  // pid -> not before
    std::wstring last_error;
    int64_t next_resolve = 0, next_text = 0;
    while (!quitting()) {
      const int64_t now = virgil::mono_ns();
      if (now >= next_resolve) {
        // Which processes to capture: the app's process-tree roots (again
        // every 2 s: apps start and quit), or our own for "all apps except
        // Virgil Control", whose output is excluded from the capture.
        next_resolve = now + 2000000000LL;
        std::vector<DWORD> pids = all ? std::vector<DWORD>{GetCurrentProcessId()} : wa::root_processes(exe);
        if (pids.size() > kMaxStreamsPerApp) pids.resize(kMaxStreamsPerApp);
        for (auto it = streams.begin(); it != streams.end();) {
          if (std::find(pids.begin(), pids.end(), it->pid) == pids.end()) {
            release_lane(it->lane);
            it->cap->close();
            it = streams.erase(it);
          } else {
            ++it;
          }
        }
        for (DWORD pid : pids) {
          if (quitting()) break;
          if (std::any_of(streams.begin(), streams.end(), [pid](const Stream& s) { return s.pid == pid; })) continue;
          auto r = retry_at.find(pid);
          if (r != retry_at.end() && now < r->second) continue;
          Stream s;
          s.pid = pid;
          s.cap.reset(new wa::Capture());
          std::wstring err;
          HRESULT hr = s.cap->open_process(pid, !all, rate_, &err);
          if (SUCCEEDED(hr)) {
            s.lane = claim_lane(i, src.channel - 1, width, s.cap->format().rate);
            if (s.lane < 0) {
              hr = E_OUTOFMEMORY;
              err = L"too many capture streams";
            } else if (FAILED(hr = s.cap->start())) {
              err = L"cannot start capturing: " + wa::hr_text(hr);
            }
          }
          if (FAILED(hr)) {
            release_lane(s.lane);
            s.cap->close();
            retry_at[pid] = now + 5000000000LL;
            last_error = err;
            continue;
          }
          retry_at.erase(pid);
          streams.push_back(std::move(s));
        }
        for (auto it = retry_at.begin(); it != retry_at.end();)  // forget processes long gone
          it = std::find(pids.begin(), pids.end(), it->first) == pids.end() ? retry_at.erase(it) : std::next(it);
        next_text = 0;
      }
      if (streams.empty()) {
        if (now >= next_text) {
          next_text = now + 250000000;
          if (!last_error.empty() && !retry_at.empty()) set_text(sh, last_error, true);
          else set_text(sh, all ? L"Starting\u2026" : src.app.empty() ? L"No app chosen" : L"Not running", !all && src.app.empty());
        }
        wait_quit(100);
        continue;
      }
      pump(streams, width);
      for (auto it = streams.begin(); it != streams.end();) {
        if (it->failed) {
          // E.g. the process quit; the next resolve tries again if it is back.
          release_lane(it->lane);
          it->cap->close();
          retry_at[it->pid] = virgil::mono_ns() + 1000000000LL;
          it = streams.erase(it);
        } else {
          ++it;
        }
      }
      if (now >= next_text && !streams.empty()) {
        next_text = now + 250000000;
        // The busiest stream speaks for the source.
        const Stream* best = &streams[0];
        for (const Stream& s : streams)
          if (lanes_[s.lane].bridge.state() == virgil::AdaptiveBridge::kRunning) best = &s;
        std::wstring t = lane_text(*best);
        if (streams.size() > 1) t += fmt(L", %u instances", unsigned(streams.size()));
        set_text(sh, t, false);
        last_error.clear();
      }
    }
    for (Stream& s : streams) {
      release_lane(s.lane);
      s.cap->close();
    }
  }

  // ---- output thread: receive channels -> a Windows device ---------------------------
  void output_loop() {
    wa::ComScope com;
    virgil::set_realtime_priority(0, 10000000);
    if (FAILED(com.hr)) {
      set_text(out_, L"Cannot start: COM failed", true);
      return;
    }
    const vc::DesktopAudioOutput& o = cfg_.output;
    if (o.channel < 1 || o.channel > rxch_) {
      set_text(out_, rxch_ ? fmt(L"Virgil has %u receive channels: choose others", rxch_)
                           : std::wstring(L"Virgil has no receive channels (Settings)"),
               true);
      return;
    }
    const std::wstring id = widen(o.device);
    const bool follows_default = id.empty() || id == L"default";
    // kInterface (WASAPI): receive channel.. onto the device's channels
    // first_output.. as they are, with a shorter buffer.
    const bool iface = o.type == vc::DesktopAudioOutput::kInterface;
    const unsigned width = iface ? o.outputs : 2;
    std::vector<float> buf;
    while (!quitting()) {
      const unsigned changes0 = watch_.changes();
      unsigned defaults = watch_.default_changes();
      wa::Render r;
      std::wstring err;
      HRESULT hr = r.open(id, iface ? 200000 : 300000, &err);  // 20 / 30 ms
      std::wstring extra;
      if (SUCCEEDED(hr) && iface) {
        const asioconv::Range rg = asioconv::select_channels(o.first_output, o.outputs, r.format().channels);
        if (rg.count == 0) {
          hr = E_INVALIDARG;
          err = fmt(L"the device has %u output%ls: choose others", r.format().channels,
                    r.format().channels == 1 ? L"" : L"s");
        } else if (rg.clamped) {
          extra = fmt(L"; the device has %u outputs", r.format().channels);
        }
      }
      if (SUCCEEDED(hr)) {
        virgil::BridgeConfig bc;
        bc.channels = width;
        bc.in_rate = rate_;
        bc.out_rate = r.format().rate;
        bc.target_s = iface ? 0.030 : 0.040;
        bc.slack_s = iface ? 0.100 : 0.200;
        bc.max_pull = r.buffer_frames() + 64;
        std::lock_guard<std::mutex> l(out_mu_);
        set_output_map(width);
        out_live_ = out_bridge_.init(bc);
        if (!out_live_) {
          hr = E_OUTOFMEMORY;
          err = L"out of memory";
        }
      }
      if (SUCCEEDED(hr)) {
        buf.assign(size_t(r.buffer_frames() + 64) * width, 0.f);
        // The echo check: loopback of this device (a kDevice source) would
        // capture what plays here, interface outputs too; an interface's
        // inputs are other channels and never echo.
        set_endpoint(out_, r.endpoint());
        hr = r.write(nullptr, 2, r.buffer_frames());  // start from a full buffer of silence
        if (SUCCEEDED(hr)) hr = r.start();
        if (FAILED(hr)) err = L"cannot start playing: " + wa::hr_text(hr);
      }
      if (FAILED(hr)) {
        {
          std::lock_guard<std::mutex> l(out_mu_);
          out_live_ = false;
        }
        r.close();
        set_text(out_, err, true);
        for (int k = 0; k < 30 && !quitting() && watch_.changes() == changes0; ++k) wait_quit(100);
        continue;
      }
      int64_t next_text = 0;
      while (!quitting()) {
        HANDLE h[2] = {quit_ev_, r.event()};
        WaitForMultipleObjects(2, h, FALSE, 100);
        if (quitting()) break;
        if (follows_default && watch_.default_changes() != defaults) {
          defaults = watch_.default_changes();
          if (wa::default_render_id() != r.endpoint()) break;
        }
        UINT32 n = 0;
        if (FAILED(hr = r.writable(&n))) break;
        if (n) {
          n = std::min<UINT32>(n, UINT32(buf.size() / width));
          // Filling the device buffer up each time: what it has played since
          // is what the bridge hands over now, so the wake-up time is the
          // consumption time.
          out_bridge_.pull(buf.data(), n, virgil::mono_ns());
          if (FAILED(hr = iface ? r.write_channels(buf.data(), width, o.first_output - 1, n) : r.write(buf.data(), 2, n)))
            break;
        }
        const int64_t now = virgil::mono_ns();
        if (now >= next_text) {
          next_text = now + 250000000;
          const bool running = out_bridge_.state() == virgil::AdaptiveBridge::kRunning;
          const std::wstring rate = rate_text(r.format().rate);
          if (iface)
            set_text(out_,
                     (running ? fmt(L"Playing to %u output%ls (%ls, WASAPI, clock %+.0f ppm)", width, width == 1 ? L"" : L"s",
                                    rate.c_str(), out_bridge_.adjust_ppm())
                              : L"Starting (" + rate + L", WASAPI)") + extra,
                     false);
          else
            set_text(out_,
                     running ? fmt(L"Playing (%ls, clock %+.0f ppm)", rate.c_str(), out_bridge_.adjust_ppm())
                             : L"Starting\u2026",
                     false);
        }
      }
      {
        std::lock_guard<std::mutex> l(out_mu_);
        out_live_ = false;
      }
      r.close();
      if (FAILED(hr)) {
        set_text(out_, wa::hr_text(hr) + L": retrying", true);
        wait_quit(500);
      }
    }
  }

  // ---- ASIO: one driver for an input source and / or the interface output --------------
  // The host's thread calls prepare() before the driver starts and the
  // driver's audio thread process() for every buffer (see asio_host.h).
  struct AsioLink final : ah::Callback {
    explicit AsioLink(Session* s) : s_(s) {}
    bool prepare(double rate, unsigned frames, unsigned inputs, unsigned outputs) override {
      return s_->asio_prepare(rate, frames, inputs, outputs);
    }
    void process(const float* in, float* out, size_t frames, int64_t end_ns) override {
      s_->asio_process(in, out, frames, end_ns);
    }
    Session* s_;
  };

  bool asio_prepare(double rate, unsigned frames, unsigned inputs, unsigned outputs) {
    // A few ms: one I/O block (the consumer's pull), two driver buffers
    // (the producer's step plus scheduling) and 2 ms.
    const double period = frames / rate;
    const double target = double(B_) / rate_ + 2 * period + 0.002;
    const double slack = std::max(0.030, target), extrapolate = std::max(0.030, 2 * period);
    if (inputs && asio_src_ >= 0) {
      asio_lane_ = claim_lane(unsigned(asio_src_), cfg_.sources[size_t(asio_src_)].channel - 1, inputs, rate, target,
                              slack, extrapolate);
      if (asio_lane_ < 0) return false;
    }
    if (outputs) {
      virgil::BridgeConfig bc;
      bc.channels = outputs;
      bc.in_rate = rate_;
      bc.out_rate = rate;
      bc.target_s = target;
      bc.slack_s = slack;
      bc.max_pull = frames + 64;
      std::lock_guard<std::mutex> l(out_mu_);
      set_output_map(outputs);
      out_live_ = out_bridge_.init(bc);
      if (!out_live_) return false;
      asio_out_.store(true, std::memory_order_release);
    }
    return true;
  }

  // The driver's audio thread: no locks, no allocation.
  void asio_process(const float* in, float* out, size_t frames, int64_t end_ns) {
    if (in && asio_lane_ >= 0) lanes_[asio_lane_].bridge.push(in, frames, end_ns);
    if (out) {
      if (asio_out_.load(std::memory_order_acquire)) out_bridge_.pull(out, frames, end_ns);
      else std::fill(out, out + frames * out_map_.size(), 0.f);
    }
  }

  void asio_cleanup() {
    release_lane(asio_lane_);
    asio_lane_ = -1;
    asio_out_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> l(out_mu_);
    out_live_ = false;
  }

  void asio_loop() {
    const vc::DesktopAudioOutput& o = cfg_.output;
    // The first ASIO input source and the interface output share the driver.
    int in = -1;
    for (size_t i = 0; i < cfg_.sources.size(); ++i) {
      const vc::DesktopAudioSource& s = cfg_.sources[i];
      if (!is_asio_input(s)) continue;
      if (in < 0) {
        in = int(i);
      } else if (s.device == cfg_.sources[size_t(in)].device) {
        set_text(src_[i], L"Only one ASIO input source per driver: give the first one more inputs", true);
      } else {
        set_text(src_[i], L"only one ASIO driver at a time: \u201C" + widen(cfg_.sources[size_t(in)].device) +
                              L"\u201D is in use by another source",
                 true);
      }
    }
    bool out = is_asio_output(o);
    if (out && in >= 0 && o.device != cfg_.sources[size_t(in)].device) {
      set_text(out_, L"only one ASIO driver at a time: \u201C" + widen(cfg_.sources[size_t(in)].device) +
                         L"\u201D is in use by an input source",
               true);
      out = false;
    }
    if (in >= 0 && (cfg_.sources[size_t(in)].channel < 1 || cfg_.sources[size_t(in)].channel > txch_)) {
      set_text(src_[size_t(in)], fmt(L"Virgil has %u transmit channels: choose others", txch_), true);
      in = -1;
    }
    if (out && (o.channel < 1 || o.channel > rxch_)) {
      set_text(out_, rxch_ ? fmt(L"Virgil has %u receive channels: choose others", rxch_)
                           : std::wstring(L"Virgil has no receive channels (Settings)"),
               true);
      out = false;
    }
    if (in < 0 && !out) return;
    const vc::DesktopAudioSource* src = in >= 0 ? &cfg_.sources[size_t(in)] : nullptr;
    auto say = [&](const std::wstring& in_text, const std::wstring& out_text, bool error) {
      if (src) set_text(src_[size_t(in)], in_text, error);
      if (out) set_text(out_, out_text, error);
    };
    if (!ah::built_in()) {
      say(ah::kNotBuiltIn, ah::kNotBuiltIn, true);
      while (!wait_quit(1000)) {}
      return;
    }
    ah::Config c;
    c.driver = widen(src ? src->device : o.device);
    c.rate = rate_;
    if (src) {
      c.first_input = src->first_input;
      c.inputs = src->inputs;
    }
    if (out) {
      c.first_output = o.first_output;
      c.outputs = o.outputs;
    }
    const unsigned bin = src ? src->buffer : 0, bout = out ? o.buffer : 0;
    c.buffer = bin ? bin : bout;
    const std::wstring buf_note =
        bin && bout && bin != bout ? fmt(L"; buffer %u of the input source used, not %u", bin, bout) : std::wstring();
    AsioLink link(this);
    asio_src_ = in;
    while (!quitting()) {
      ah::Host host;
      std::wstring err;
      asio_lane_ = -1;
      asio_out_.store(false);
      if (!host.open(c, &link, &err)) {
        asio_cleanup();
        say(err, err, true);
        for (int k = 0; k < 30 && !quitting(); ++k) wait_quit(100);  // again in 3 s
        continue;
      }
      const unsigned rate = unsigned(std::lround(host.rate()));
      const std::wstring fmt_text =
          rate_text(rate) + (rate != rate_ ? L", resampled" : L"") + fmt(L", ASIO buffer %u", host.buffer());
      std::wstring in_extra = host.note().empty() ? std::wstring() : L"; " + host.note(), out_extra = in_extra;
      in_extra += buf_note;
      if (src) {
        in_extra += tx_overflow(*src, host.inputs());
        if (host.inputs_clamped()) in_extra += fmt(L"; the interface has %u inputs", host.device_inputs());
      }
      if (out && host.outputs_clamped()) out_extra += fmt(L"; the interface has %u outputs", host.device_outputs());
      std::wstring why;
      uint64_t seen = host.buffers();
      int64_t progress = virgil::mono_ns(), next_text = 0;
      while (!quitting()) {
        wait_quit(100);
        if (quitting()) break;
        const int64_t now = virgil::mono_ns();
        if (host.reset_requested()) {
          why = L"The ASIO driver asked for a reset: restarting";
          break;
        }
        if (host.buffers() != seen) {
          seen = host.buffers();
          progress = now;
        } else if (now - progress > 2000000000LL) {
          why = L"The ASIO driver stopped delivering audio (interface unplugged?): restarting";
          break;
        }
        if (now < next_text) continue;
        next_text = now + 250000000;
        std::wstring it, ot;
        if (src && asio_lane_ >= 0) {
          const virgil::AdaptiveBridge& b = lanes_[asio_lane_].bridge;
          const unsigned n = host.inputs();
          it = b.state() == virgil::AdaptiveBridge::kRunning
                   ? fmt(L"Capturing %u input%ls (%ls, clock %+.0f ppm)", n, n == 1 ? L"" : L"s", fmt_text.c_str(),
                         b.adjust_ppm())
                   : L"Starting (" + fmt_text + L")";
          it += in_extra;
        }
        if (out) {
          const unsigned n = host.outputs();
          ot = out_bridge_.state() == virgil::AdaptiveBridge::kRunning
                   ? fmt(L"Playing to %u output%ls (%ls, clock %+.0f ppm)", n, n == 1 ? L"" : L"s", fmt_text.c_str(),
                         out_bridge_.adjust_ppm())
                   : L"Starting (" + fmt_text + L")";
          ot += out_extra;
        }
        say(it, ot, false);
      }
      host.close();  // no more callbacks after this
      asio_cleanup();
      if (!why.empty()) {
        say(why, why, true);
        wait_quit(why.find(L"reset") != std::wstring::npos ? 200 : 1000);
      }
    }
  }

  virgil::Client& c_;
  const vc::DesktopAudioSettings cfg_;
  wa::DeviceWatcher& watch_;
  uint32_t rate_ = 48000, txch_ = 0, rxch_ = 0, ring_ = 0, tx_lead_ = 0, rx_lat_ = 0, B_ = 240;
  Lane lanes_[kMaxLanes];
  std::unique_ptr<Shared[]> src_;
  Shared out_;
  std::mutex out_mu_;  // the I/O thread try-locks it per block
  bool out_live_ = false;
  std::vector<unsigned> out_map_;  // guarded by out_mu_ (and fixed while an ASIO host runs)
  // ASIO: set before the driver starts, read by its audio thread.
  int asio_src_ = -1, asio_lane_ = -1;
  std::atomic<bool> asio_out_{false};
  virgil::AdaptiveBridge out_bridge_;
  std::atomic<bool> quit_{false}, lost_{false};
  HANDLE quit_ev_ = nullptr;
  std::mutex st_mu_;
  std::thread io_;
  std::vector<std::thread> workers_;
  // I/O thread only.
  std::vector<float> mix_, tmp_, rxbuf_;
  bool tx_active_ = false;
  int64_t last_sound_ns_ = 0;
};

}  // namespace

// ---- Engine ---------------------------------------------------------------------------

struct Engine::Impl {
  std::mutex mu;  // guards everything below except the thread
  vc::DesktopAudioSettings want, applied;
  bool changed = false, started = false;
  Status st;
  std::unique_ptr<Session> session;
  std::atomic<bool> quit{false};
  HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::thread th;

  ~Impl() {
    if (wake) CloseHandle(wake);
  }

  void set_phase(Status::Phase p, const std::wstring& text) {
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

  void run() {
    wa::ComScope com;
    wa::DeviceWatcher watch;
    watch.start();
    virgil::Client client;
    vc::DesktopAudioSettings cur;
    int64_t retry_at = 0;
    uint32_t names_seq = ~0u;
    while (!quit.load(std::memory_order_acquire)) {
      WaitForSingleObject(wake, 250);
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
        set_phase(Status::kOff, cur.enabled ? L"Add a source to send Windows audio to Dante." : L"Desktop audio is off.");
        continue;
      }
      const int64_t now = virgil::mono_ns();
      if (session) {
        if (session->lost() || !client.heartbeat_fresh(kStaleNs)) {
          teardown(client);
          client.close();
          publish_layout(client, false);
          set_phase(Status::kWaiting, L"Virgil stopped or restarted: reconnecting\u2026");
          retry_at = now + 500000000LL;
          continue;
        }
        if (client.names_seq() != names_seq) {
          names_seq = client.names_seq();
          publish_layout(client, true);
        }
        set_phase(Status::kRunning, fmt(L"On: %u source%ls to Virgil%ls (%ls).", unsigned(cur.sources.size()),
                                        cur.sources.size() == 1 ? L"" : L"s",
                                        cur.output.enabled ? L", receive channels to Windows" : L"",
                                        rate_text(client.sample_rate()).c_str()));
        continue;
      }
      if (now < retry_at) continue;
      retry_at = now + 1000000000LL;
      if (!client.is_open() && !client.open()) {
        publish_layout(client, false);
        set_phase(Status::kWaiting, L"Waiting for Virgil: " + widen(client.open_error()));
        continue;
      }
      if (!client.daemon_alive(500000000LL)) {
        const bool stopped = client.header()->state.load(std::memory_order_acquire) == virgil::kStateStopped;
        set_phase(Status::kWaiting, stopped ? L"Virgil is stopped. Desktop audio starts again with Virgil."
                                            : L"Waiting for Virgil: the service is not responding.");
        // Closed so the next try finds a new soundcard (restart, new layout).
        if (!client.heartbeat_fresh(kStaleNs) || stopped) client.close();
        publish_layout(client, false);
        continue;
      }
      names_seq = client.names_seq();
      publish_layout(client, true);
      if (!cur.sources.empty() && !client.acquire_tx_slot("Desktop audio")) {
        set_phase(Status::kError, fmt(L"All %u Virgil playback slots are in use: close an app that plays to Virgil.",
                                      virgil::kMaxTxClients));
        client.close();
        retry_at = now + 5000000000LL;
        continue;
      }
      if (!cur.sources.empty()) client.clear_pending_tx();
      std::unique_ptr<Session> s(new Session(client, cur, watch));
      s->start();
      {
        std::lock_guard<std::mutex> l(mu);
        session = std::move(s);
      }
      set_phase(Status::kRunning, L"Starting\u2026");
    }
    teardown(client);
    client.close();
    watch.stop();
  }
};

Engine::Engine() : impl_(new Impl) {}
Engine::~Engine() { shutdown(); }

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
  SetEvent(impl_->wake);
}

void Engine::apply(const vc::DesktopAudioSettings& s) {
  {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->want = s;
    impl_->changed = true;
  }
  SetEvent(impl_->wake);
}

void Engine::shutdown() {
  if (!impl_) return;
  impl_->quit.store(true, std::memory_order_release);
  SetEvent(impl_->wake);
  if (impl_->th.joinable()) impl_->th.join();
}

vc::DesktopAudioSettings Engine::settings() {
  std::lock_guard<std::mutex> l(impl_->mu);
  return impl_->changed ? impl_->want : impl_->applied;
}

Status Engine::status() {
  std::lock_guard<std::mutex> l(impl_->mu);
  Status s = impl_->st;
  s.app_capture_supported = wa::process_loopback_supported();
  if (impl_->changed) {
    // New settings not picked up by the supervisor yet (a fraction of a
    // second): the running session's sources may not match them.
    s.sources.assign(impl_->want.sources.size(), SourceStatus());
    for (auto& src : s.sources) src.text = L"Starting\u2026";
    s.output.text = L"Starting\u2026";
  } else if (impl_->session) {
    impl_->session->collect(&s);
  } else {
    // Not running: every source just waits.
    const vc::DesktopAudioSettings& cfg = impl_->changed ? impl_->want : impl_->applied;
    s.sources.assign(cfg.sources.size(), SourceStatus());
    for (auto& src : s.sources) src.text = cfg.enabled ? L"Waiting for Virgil" : L"Off";
    s.output.text = cfg.output.enabled && cfg.enabled ? L"Waiting for Virgil" : L"Off";
  }
  return s;
}

// ---- settings file --------------------------------------------------------------------

std::wstring settings_path() {
  wchar_t dir[MAX_PATH];
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dir))) return L"desktop-audio.conf";
  return std::wstring(dir) + L"\\Virgil\\desktop-audio.conf";
}

vc::DesktopAudioSettings load_settings(std::wstring* error) {
  vc::DesktopAudioSettings s;
  const std::wstring path = settings_path();
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    if (error && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
      *error = L"cannot read " + path;
    return s;
  }
  std::string text;
  char buf[4096];
  DWORD n = 0;
  while (text.size() < (1u << 20) && ReadFile(f, buf, sizeof buf, &n, nullptr) && n) text.append(buf, n);
  CloseHandle(f);
  std::vector<std::string> warnings;
  vc::parse_desktop_audio(text, &s, &warnings);
  if (error && !warnings.empty()) *error = widen(warnings.front());
  return s;
}

bool save_settings(const vc::DesktopAudioSettings& s, std::wstring* error) {
  const std::wstring path = settings_path();
  const std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
  CreateDirectoryW(dir.c_str(), nullptr);
  // Written next to it, then moved over it: never a half-written file.
  const std::wstring tmp = path + L".tmp";
  const std::string text = vc::format_desktop_audio(s);
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    if (error) *error = L"cannot write " + tmp;
    return false;
  }
  DWORD n = 0;
  const bool ok = WriteFile(f, text.data(), DWORD(text.size()), &n, nullptr) && n == text.size() && FlushFileBuffers(f);
  CloseHandle(f);
  if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp.c_str());
    if (error) *error = L"cannot save " + path;
    return false;
  }
  return true;
}

}  // namespace da
