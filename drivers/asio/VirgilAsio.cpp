// ASIO driver for the Virgil virtual soundcard (Windows, x64).
//
// An in-process COM object implementing IASIO. The buffer-switch thread is
// clocked directly by the virgild media clock (PTP), so ASIO hosts run in lock
// step with the Dante network: no resampling, no drift.
//
// Latency per direction = ASIO buffer + virgild receive latency (input) or
// ASIO buffer + virgild transmit lead + one packet (output), reported exactly
// through getLatencies().
//
// Build needs the Steinberg ASIO SDK (common/asio.h, common/iasiodrv.h):
//   cmake -DASIO_SDK_DIR=C:/path/to/asiosdk ...
// Register:   regsvr32 VirgilAsio.dll     (as administrator)
#include <windows.h>
#include <objbase.h>
#include <olectl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

#include "virgil/client.h"
#include "virgil/platform.h"
#include "virgil/sample_convert.h"

// {ACDDF2EF-FA26-401C-9CA6-BA447CEA8D38}
static const CLSID CLSID_VirgilAsio = {
    0xacddf2ef, 0xfa26, 0x401c, {0x9c, 0xa6, 0xba, 0x44, 0x7c, 0xea, 0x8d, 0x38}};
static const char kClsidString[] = "{ACDDF2EF-FA26-401C-9CA6-BA447CEA8D38}";
static const char kDriverName[] = "Virgil";

static HINSTANCE g_module = nullptr;
static std::atomic<long> g_objects{0};
static std::atomic<long> g_locks{0};
static std::atomic<long> g_threads{0};  // buffer threads still running (maybe detached)

namespace {

void to_asio64(int64_t v, unsigned long* hi, unsigned long* lo) {
  *hi = static_cast<unsigned long>(uint64_t(v) >> 32);
  *lo = static_cast<unsigned long>(uint64_t(v) & 0xffffffffu);
}

// Lifecycle and threading model
// -----------------------------
// Hosts call stop()/disposeBuffers()/init()/Release() from any thread, and
// some do it synchronously from inside our callbacks (bufferSwitch, or the
// kAsioResetRequest/kAsioResyncRequest asioMessage), i.e. on the buffer thread
// itself. So:
//  * Each start() creates a Run: a generation token (`go`) plus a reference to
//    the Session (buffers + a copy of the host callbacks). The buffer thread
//    only uses its own Run, so disposeBuffers() never frees memory the thread
//    still reads: the Session lives until the last Run using it is gone.
//  * stop() clears the Run's `go` and retires its thread. A thread is joined
//    by whichever later call runs on another thread (stop, start, init,
//    disposeBuffers, the destructor); it never joins itself.
//  * After every host callback returns, the buffer thread re-checks `go`
//    before it touches the driver again: when the callback stopped us (or
//    even released the last reference and deleted the driver), it leaves
//    without touching `this`.
//  * A new Run waits for every retired thread to have finished before its
//    first callback, so the host never sees two buffer threads at once.
//  * The lifecycle lock is never held while joining or while calling the
//    host, so callbacks may re-enter the driver freely.
class VirgilAsio : public IASIO {
 public:
  VirgilAsio() { ++g_objects; }
  virtual ~VirgilAsio() {
    {
      Lock l(mu_);
      stop_locked();
      session_.reset();
    }
    // Released from inside one of our own callbacks: that thread is still
    // running (and leaves without touching us, its Run is stopped), so it
    // cannot be joined here; detach it. Every other thread is joined.
    std::vector<Worker> all;
    {
      Lock l(mu_);
      all.swap(retired_);
    }
    for (auto& w : all) {
      if (!w.th.joinable()) continue;
      if (w.th.get_id() == std::this_thread::get_id()) w.th.detach();
      else w.th.join();
    }
    if (client_.is_open()) client_.set_tx_active(false);
    --g_objects;
  }

  // IUnknown. ASIO hosts pass the driver CLSID as the IID.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, CLSID_VirgilAsio)) {
      *ppv = static_cast<IASIO*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const long r = --refs_;
    if (r == 0) delete this;
    return ULONG(r);
  }

  // IASIO
  ASIOBool init(void*) override {
    // A host may init() again (e.g. after a reset request) without disposing
    // first: tear the old stream down before the client is reopened, the
    // buffer thread reads its shared memory.
    disposeBuffers();
    Lock l(mu_);
    if (!client_.open()) {
      set_error(client_.open_error().empty() ? "Cannot open the Virgil soundcard" : client_.open_error().c_str());
      return ASIOFalse;
    }
    if (!client_.daemon_alive()) {
      const virgil::ShmHeader* h = client_.header();
      if (h->state.load(std::memory_order_acquire) == virgil::kStateStopped)
        set_error("Virgil is stopped: press Start in Virgil Control");
      else
        set_error("the Virgil service is not responding (its audio engine stopped ticking): restart the Virgil "
                  "service, see Virgil Control");
      client_.close();
      return ASIOFalse;
    }
    // Name the slot after the host (e.g. "REAPER"), shown in Virgil Control.
    char exe[MAX_PATH] = "", name[40] = "ASIO";
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (const char* base = std::strrchr(exe, '\\')) {
      std::snprintf(name, sizeof name, "%s", base + 1);
      if (char* dot = std::strrchr(name, '.')) *dot = 0;
    }
    if (!client_.acquire_tx_slot(name)) {
      set_error("All Virgil playback slots are in use.");
      client_.close();
      return ASIOFalse;
    }
    in_ch_ = long(client_.rx_channels());
    out_ch_ = long(client_.tx_channels());
    error_[0] = 0;
    return ASIOTrue;
  }
  void getDriverName(char* name) override { std::strcpy(name, "Virgil ASIO"); }
  long getDriverVersion() override { return 1; }
  // The ASIO spec gives the host a 124-byte buffer: never write more.
  void getErrorMessage(char* s) override {
    Lock l(mu_);
    constexpr size_t kAsioErrorBytes = 124;
    size_t n = std::strlen(error_);
    if (n < kAsioErrorBytes) {
      std::memcpy(s, error_, n + 1);
      return;
    }
    // Cut on a word boundary and mark it; the full text went to the debugger.
    n = kAsioErrorBytes - 4;
    while (n > 0 && error_[n] != ' ') --n;
    while (n > 0 && (error_[n - 1] == ' ' || error_[n - 1] == ',' || error_[n - 1] == ':')) --n;
    if (n == 0) n = kAsioErrorBytes - 4;
    std::memcpy(s, error_, n);
    std::memcpy(s + n, "...", 4);
  }

  ASIOError start() override {
    // Join threads stopped earlier (from other threads than this one) first.
    reap();
    Lock l(mu_);
    if (!session_ || !client_.is_open()) return ASE_NotPresent;
    if (run_) return ASE_OK;  // already running: idempotent
    if (!client_.daemon_alive()) return ASE_HWMalfunction;
    auto r = std::make_shared<Run>();
    r->session = session_;
    // Earlier generations still finishing (stopped from inside their own
    // callback, or being joined by another thread right now): the new thread
    // waits for them before it calls the host.
    runs_.erase(std::remove_if(runs_.begin(), runs_.end(),
                               [](const std::shared_ptr<Run>& p) {
                                 return p->exited.load(std::memory_order_acquire);
                               }),
                runs_.end());
    r->previous = runs_;
    client_.clear_pending_tx();  // nothing from before the last stop may replay
    client_.set_tx_active(true);
    position_ = 0;
    try {
      ++g_threads;
      thread_ = std::thread([this, r] {
        run(r);
        // Nothing of the driver may be touched past this point: it may be gone.
        r->exited.store(true, std::memory_order_release);
        --g_threads;
      });
    } catch (...) {
      --g_threads;
      client_.set_tx_active(false);
      return ASE_HWMalfunction;
    }
    runs_.push_back(r);
    run_ = std::move(r);
    running_ = true;
    return ASE_OK;
  }

  ASIOError stop() override {
    {
      Lock l(mu_);
      stop_locked();
    }
    reap();
    return ASE_OK;
  }

  ASIOError getChannels(long* in, long* out) override {
    Lock l(mu_);
    if (!client_.is_open()) return ASE_NotPresent;
    *in = in_ch_;
    *out = out_ch_;
    return ASE_OK;
  }

  ASIOError getLatencies(long* in, long* out) override {
    Lock l(mu_);
    if (!client_.is_open()) return ASE_NotPresent;
    const long b = long(session_ ? session_->frames : kPreferred);
    *in = b + long(client_.rx_latency_frames());
    *out = b + long(client_.tx_lead_frames() + client_.period_frames());
    return ASE_OK;
  }

  ASIOError getBufferSize(long* minSize, long* maxSize, long* preferred, long* granularity) override {
    *minSize = kMin;
    *maxSize = kMax;
    *preferred = kPreferred;
    *granularity = kGranularity;
    return ASE_OK;
  }

  ASIOError canSampleRate(ASIOSampleRate rate) override {
    Lock l(mu_);
    if (!client_.is_open()) return ASE_NotPresent;
    return std::fabs(rate - double(client_.sample_rate())) < 0.5 ? ASE_OK : ASE_NoClock;
  }
  ASIOError getSampleRate(ASIOSampleRate* rate) override {
    Lock l(mu_);
    if (!client_.is_open()) return ASE_NotPresent;
    *rate = client_.sample_rate();
    return ASE_OK;
  }
  ASIOError setSampleRate(ASIOSampleRate rate) override {
    // 0 means "external sync": we always are (PTP).
    if (rate == 0) return ASE_OK;
    return canSampleRate(rate);
  }

  ASIOError getClockSources(ASIOClockSource* clocks, long* num) override {
    if (!clocks || !num || *num < 1) return ASE_InvalidParameter;
    std::memset(clocks, 0, sizeof *clocks);
    clocks->index = 0;
    clocks->associatedChannel = -1;
    clocks->associatedGroup = -1;
    clocks->isCurrentSource = ASIOTrue;
    std::strcpy(clocks->name, "Dante PTP");
    *num = 1;
    return ASE_OK;
  }
  ASIOError setClockSource(long ref) override { return ref == 0 ? ASE_OK : ASE_InvalidParameter; }

  // Called from the host's callback on the buffer thread: lock free.
  ASIOError getSamplePosition(ASIOSamples* pos, ASIOTimeStamp* ts) override {
    if (!running_) return ASE_SPNotAdvancing;
    to_asio64(position_.load(), &pos->hi, &pos->lo);
    to_asio64(switch_ns_.load(), &ts->hi, &ts->lo);
    return ASE_OK;
  }

  ASIOError getChannelInfo(ASIOChannelInfo* info) override {
    Lock l(mu_);
    const bool in = info->isInput != ASIOFalse;
    if (info->channel < 0 || info->channel >= (in ? in_ch_ : out_ch_)) return ASE_InvalidParameter;
    info->type = ASIOSTInt32LSB;
    info->channelGroup = 0;
    info->isActive = ASIOFalse;
    if (session_)
      for (const auto& c : (in ? session_->inputs : session_->outputs))
        if (c.channel == info->channel) info->isActive = ASIOTrue;
    // The channel's name in Dante Controller when it has one ("3 Kick In").
    const std::string dante = client_.channel_name(!in, uint32_t(info->channel));
    const std::string dflt = std::string(in ? "RX " : "TX ") + std::to_string(info->channel + 1);
    if (!dante.empty() && dante != dflt)
      std::snprintf(info->name, sizeof info->name, "%ld %s", info->channel + 1, dante.c_str());
    else
      std::snprintf(info->name, sizeof info->name, "Virgil %s %ld", in ? "In" : "Out", info->channel + 1);
    return ASE_OK;
  }

  ASIOError createBuffers(ASIOBufferInfo* infos, long n, long size, ASIOCallbacks* cb) override {
    // Replaces any previous buffers (and stops their stream).
    disposeBuffers();
    if (n < 0 || (n > 0 && !infos)) return ASE_InvalidParameter;
    if (size < kMin || size > kMax || !cb) return ASE_InvalidMode;
    if (!(cb->bufferSwitch || cb->bufferSwitchTimeInfo)) return ASE_InvalidMode;
    auto s = std::make_shared<Session>();
    {
      Lock l(mu_);
      if (!client_.is_open()) return ASE_NotPresent;
      if (size > long(client_.ring_frames() / 4)) return ASE_InvalidMode;
      for (long i = 0; i < n; ++i) {
        const ASIOBufferInfo& bi = infos[i];
        const bool in = bi.isInput != ASIOFalse;
        if (bi.channelNum < 0 || bi.channelNum >= (in ? in_ch_ : out_ch_)) return ASE_InvalidParameter;
      }
    }
    try {
      for (long i = 0; i < n; ++i) {
        Channel c;
        c.channel = infos[i].channelNum;
        c.data.assign(size_t(size) * 2, 0);
        (infos[i].isInput != ASIOFalse ? s->inputs : s->outputs).push_back(std::move(c));
      }
    } catch (...) {
      return ASE_NoMemory;
    }
    // Hand out the double-buffer halves once all channels are allocated.
    size_t ii = 0, oo = 0;
    for (long i = 0; i < n; ++i) {
      Channel& c = infos[i].isInput != ASIOFalse ? s->inputs[ii++] : s->outputs[oo++];
      infos[i].buffers[0] = c.data.data();
      infos[i].buffers[1] = c.data.data() + size;
    }
    s->frames = uint32_t(size);
    s->cb = *cb;  // our own copy: the thread never reads host memory for it
    // Asked without the lock held: the host may call back into the driver.
    s->time_info = cb->bufferSwitchTimeInfo && cb->asioMessage &&
                   cb->asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) == 1 &&
                   cb->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
    if (!s->time_info && !s->cb.bufferSwitch) return ASE_InvalidMode;
    Lock l(mu_);
    session_ = std::move(s);
    return ASE_OK;
  }

  ASIOError disposeBuffers() override {
    {
      Lock l(mu_);
      stop_locked();
      // A thread still running (stopped from inside its own callback) keeps
      // the session alive through its Run; the memory goes when it exits.
      session_.reset();
    }
    reap();
    return ASE_OK;
  }

  ASIOError controlPanel() override {
    MessageBoxA(nullptr,
                "Virgil is configured in virgil.conf (sample rate, channels, latency; routing in Dante Controller).\n"
                "Run 'virgild --status' to see clock and network state.",
                kDriverName, MB_OK | MB_ICONINFORMATION);
    return ASE_OK;
  }

  ASIOError future(long selector, void*) override {
    switch (selector) {
      case kAsioCanTimeInfo:
        return ASE_SUCCESS;
      default:
        return ASE_InvalidParameter;
    }
  }

  ASIOError outputReady() override { return ASE_NotPresent; }

 private:
  using Lock = std::lock_guard<std::recursive_mutex>;

  struct Channel {
    long channel = 0;
    std::vector<int32_t> data;  // two halves of `frames` each
  };
  // What createBuffers() set up. Its layout is fixed once published; only the
  // sample data changes (written by the buffer thread and the host).
  struct Session {
    std::vector<Channel> inputs, outputs;
    uint32_t frames = 0;
    ASIOCallbacks cb{};
    bool time_info = false;
  };
  // One start()..stop() generation of the buffer thread.
  struct Run {
    std::shared_ptr<Session> session;
    std::atomic<bool> go{true};
    std::atomic<bool> exited{false};
    std::vector<std::shared_ptr<Run>> previous;  // must exit before our first callback
  };
  struct Worker {
    std::thread th;
    std::shared_ptr<Run> run;
  };

  static constexpr long kMin = 32;
  static constexpr long kMax = 2048;
  static constexpr long kPreferred = 128;
  static constexpr long kGranularity = 16;

  // Shown by the host via getErrorMessage (shortened to the ASIO limit) and
  // written in full to the debugger output (DebugView). No message box: hosts
  // init() every driver while scanning.
  void set_error(const char* s) {
    std::snprintf(error_, sizeof error_, "%s", s);
    if (error_[0] >= 'a' && error_[0] <= 'z') error_[0] = char(error_[0] - 'a' + 'A');
    char line[sizeof error_ + 32];
    std::snprintf(line, sizeof line, "Virgil ASIO: init failed: %s\n", error_);
    OutputDebugStringA(line);
  }

  // Ends the current generation (if any) and retires its thread. Never joins:
  // that happens in reap(), without the lock. Caller holds mu_.
  void stop_locked() {
    if (run_) {
      run_->go.store(false, std::memory_order_release);
      retired_.push_back(Worker{std::move(thread_), std::move(run_)});
      run_.reset();
    }
    running_ = false;
    if (client_.is_open()) client_.set_tx_active(false);
  }

  // Joins every retired buffer thread except the calling one (a thread
  // cannot join itself; it is left for a later call from another thread).
  void reap() {
    std::vector<Worker> joinable;
    {
      Lock l(mu_);
      for (size_t i = 0; i < retired_.size();) {
        Worker& w = retired_[i];
        if (!w.th.joinable()) {
          retired_.erase(retired_.begin() + long(i));
        } else if (w.th.get_id() != std::this_thread::get_id()) {
          joinable.push_back(std::move(w));
          retired_.erase(retired_.begin() + long(i));
        } else {
          ++i;
        }
      }
    }
    for (auto& w : joinable) w.th.join();
  }

  // Buffer-switch thread: wakes on media-clock buffer boundaries. It reads the
  // driver only while r->go holds; the host's callbacks may clear it (or even
  // delete the driver), so it is re-checked after each one before going on.
  void run(const std::shared_ptr<Run>& r) {
    // Let threads stopped from inside their own callback finish first.
    for (const auto& p : r->previous)
      while (!p->exited.load(std::memory_order_acquire)) {
        if (!r->go.load(std::memory_order_acquire)) return;
        virgil::sleep_until_ns(virgil::mono_ns() + 1000000);
      }
    r->previous.clear();
    if (!r->go.load(std::memory_order_acquire)) return;

    Session& s = *r->session;
    const ASIOCallbacks& cb = s.cb;
    const uint32_t B = s.frames;
    const uint32_t rate = client_.sample_rate();
    if (B == 0 || rate == 0) return;
    virgil::set_realtime_priority(0, int64_t(B) * 1000000000LL / rate);
    const uint32_t rx_lat = client_.rx_latency_frames();
    const uint32_t out_off = B + client_.tx_lead_frames();
    const uint32_t rxch = client_.rx_channels(), txch = client_.tx_channels();
    auto alive = [&r] { return r->go.load(std::memory_order_acquire); };

    double now = 0;
    client_.frame_now(&now);
    uint64_t t = (uint64_t(now) / B + 1) * B;  // first boundary
    uint64_t k = 0;
    ASIOTime time_info{};
    uint64_t steps = client_.header()->clock_steps.load();
    bool reset_requested = false;
    // How far the next callback may sit from "now" before we realign: a
    // couple of buffers ahead, a quarter ring behind.
    const uint64_t max_ahead = 2 * uint64_t(B) + rate / 20;
    const uint64_t max_behind = client_.ring_frames() / 4;
    const int64_t max_sleep_ns = 20000000;

    while (alive()) {
      // virgild gone for >2 s (settings changed the soundcard layout, or it was
      // restarted): ask the host to re-initialise us so init() reconnects.
      // Shorter gaps (settings applied with the same layout) ride through on
      // the extrapolated clock. Hosts often stop/dispose/release us right in
      // this callback.
      if (!reset_requested && !client_.heartbeat_fresh(2000000000LL) && cb.asioMessage) {
        reset_requested = true;
        if (cb.asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr)) {
          if (!alive()) return;
          cb.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        }
        if (!alive()) return;
      }
      // Step count before the anchor (the daemon counts a step once the new
      // anchor is out), so a new count always comes with the new anchor.
      const uint64_t cur_steps = client_.header()->clock_steps.load(std::memory_order_acquire);
      virgil::ClockAnchor a;
      if (!client_.anchor(&a)) {
        // The daemon is mid-update (its tick thread was preempted while
        // publishing): try again shortly. Leaving the loop here would end
        // playback for good while the host keeps waiting for callbacks.
        virgil::sleep_until_ns(virgil::mono_ns() + 1000000);
        continue;
      }
      const int64_t now_ns = virgil::mono_ns();
      const double now_frame = a.frame_at(now_ns);
      // Realign when the timeline moved (PTP relock, engine restarted with a
      // new clock) or when we are far off it for any other reason: a
      // callback that is due years from now, or one so late that the rings
      // have wrapped, would otherwise silence playback until the host
      // restarts the driver.
      if (cur_steps != steps || double(t) > now_frame + double(max_ahead) ||
          double(t) + double(max_behind) < now_frame) {
        steps = cur_steps;
        t = (uint64_t(now_frame) / B + 1) * B;
        if (cb.asioMessage) {
          cb.asioMessage(kAsioResyncRequest, 0, nullptr, nullptr);
          if (!alive()) return;
        }
      }
      const int64_t deadline = a.host_ns_at(double(t));
      if (deadline - now_ns > max_sleep_ns) {
        // Never commit to a long sleep: the anchor may change under us.
        virgil::sleep_until_ns(now_ns + max_sleep_ns);
        continue;
      }
      virgil::sleep_until_ns(deadline, 50000);
      if (!alive()) return;

      const long idx = long(k & 1);
      // Capture: the B frames ending rx_lat before now.
      const uint64_t in_start = t - B - rx_lat;
      for (auto& c : s.inputs) {
        int32_t* dst = c.data.data() + size_t(idx) * B;
        if (uint32_t(c.channel) >= rxch) continue;
        for (uint32_t f = 0; f < B; ++f)
          dst[f] = virgil::float_to_s32(client_.rx_frame(in_start + f)[c.channel]);
      }

      position_ = int64_t(k) * B;
      switch_ns_ = deadline;
      if (s.time_info) {
        std::memset(&time_info, 0, sizeof time_info);
        AsioTimeInfo& ti = time_info.timeInfo;
        ti.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
        ti.speed = 1.0;
        ti.sampleRate = rate;
        to_asio64(int64_t(k) * B, &ti.samplePosition.hi, &ti.samplePosition.lo);
        to_asio64(deadline, &ti.systemTime.hi, &ti.systemTime.lo);
        cb.bufferSwitchTimeInfo(&time_info, idx, ASIOTrue);
      } else {
        cb.bufferSwitch(idx, ASIOTrue);
      }
      // The host may have stopped, disposed or released us in there.
      if (!alive()) return;

      // Playback: the host filled half `idx`; it plays one buffer later.
      // Frames whose slot the daemon already consumed (we woke very late)
      // are dropped rather than left to replay a ring later.
      const uint64_t out_start = t + out_off;
      const uint64_t horizon = client_.tx_horizon();
      if (out_start < horizon) client_.note_late(std::min<uint64_t>(B, horizon - out_start));
      for (uint32_t f = 0; f < B; ++f) {
        if (out_start + f < horizon) continue;
        float* dst = client_.tx_frame(out_start + f);
        for (uint32_t c = 0; c < txch; ++c) dst[c] = 0.f;
        for (const auto& c : s.outputs)
          if (uint32_t(c.channel) < txch)
            dst[c.channel] = virgil::s32_to_float(c.data[size_t(idx) * B + f]);
      }
      client_.note_written(out_start + B);
      client_.touch();
      t += B;
      ++k;
    }
  }

  std::atomic<long> refs_{1};
  // Guards everything below except the atomics; never held while joining a
  // thread or calling the host. Recursive so a host re-entering the driver
  // from a callback made under it cannot deadlock.
  std::recursive_mutex mu_;
  virgil::Client client_;
  char error_[512] = "";
  long in_ch_ = 0, out_ch_ = 0;
  std::shared_ptr<Session> session_;  // set by createBuffers()
  std::shared_ptr<Run> run_;          // current generation, null when stopped
  std::thread thread_;                // runs run_
  std::vector<Worker> retired_;       // stopped threads not joined yet
  std::vector<std::shared_ptr<Run>> runs_;  // generations that may not have exited
  std::atomic<bool> running_{false};
  std::atomic<int64_t> position_{0};
  std::atomic<int64_t> switch_ns_{0};
};

class ClassFactory : public IClassFactory {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
      *ppv = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }   // static lifetime
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* d = new (std::nothrow) VirgilAsio;
    if (!d) return E_OUTOFMEMORY;
    HRESULT hr = d->QueryInterface(riid, ppv);
    d->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
    lock ? ++g_locks : --g_locks;
    return S_OK;
  }
};

ClassFactory g_factory;

bool set_reg(HKEY root, const std::string& key, const char* name, const std::string& value) {
  HKEY h;
  if (RegCreateKeyExA(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) !=
      ERROR_SUCCESS)
    return false;
  const LONG r = RegSetValueExA(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                DWORD(value.size() + 1));
  RegCloseKey(h);
  return r == ERROR_SUCCESS;
}

}  // namespace

extern "C" {

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = inst;
    DisableThreadLibraryCalls(inst);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
  if (!IsEqualCLSID(clsid, CLSID_VirgilAsio)) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return (g_objects == 0 && g_locks == 0 && g_threads == 0) ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
  char path[MAX_PATH];
  if (!GetModuleFileNameA(g_module, path, MAX_PATH)) return SELFREG_E_CLASS;
  const std::string clsid_key = std::string("CLSID\\") + kClsidString;
  bool ok = set_reg(HKEY_CLASSES_ROOT, clsid_key, nullptr, std::string(kDriverName) + " ASIO");
  ok &= set_reg(HKEY_CLASSES_ROOT, clsid_key + "\\InprocServer32", nullptr, path);
  ok &= set_reg(HKEY_CLASSES_ROOT, clsid_key + "\\InprocServer32", "ThreadingModel", "Apartment");
  const std::string asio_key = std::string("SOFTWARE\\ASIO\\") + kDriverName;
  ok &= set_reg(HKEY_LOCAL_MACHINE, asio_key, "CLSID", kClsidString);
  ok &= set_reg(HKEY_LOCAL_MACHINE, asio_key, "Description", kDriverName);
  return ok ? S_OK : SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer() {
  const std::string clsid_key = std::string("CLSID\\") + kClsidString;
  RegDeleteTreeA(HKEY_CLASSES_ROOT, clsid_key.c_str());
  RegDeleteTreeA(HKEY_LOCAL_MACHINE, (std::string("SOFTWARE\\ASIO\\") + kDriverName).c_str());
  return S_OK;
}

}  // extern "C"
