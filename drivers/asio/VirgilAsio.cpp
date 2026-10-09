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

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
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

namespace {

void to_asio64(int64_t v, unsigned long* hi, unsigned long* lo) {
  *hi = static_cast<unsigned long>(uint64_t(v) >> 32);
  *lo = static_cast<unsigned long>(uint64_t(v) & 0xffffffffu);
}

class VirgilAsio : public IASIO {
 public:
  VirgilAsio() { ++g_objects; }
  virtual ~VirgilAsio() {
    stop();
    disposeBuffers();
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
    if (!client_.open()) {
      set_error("Cannot open the Virgil soundcard: is the Virgil service running?");
      return ASIOFalse;
    }
    if (!client_.daemon_alive()) {
      set_error("The Virgil service is not running its audio engine. Restart it and reopen the driver.");
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
    return ASIOTrue;
  }
  void getDriverName(char* name) override { std::strcpy(name, "Virgil ASIO"); }
  long getDriverVersion() override { return 1; }
  void getErrorMessage(char* s) override { std::strcpy(s, error_); }

  ASIOError start() override {
    if (!callbacks_ || buffer_frames_ == 0) return ASE_NotPresent;
    if (running_) return ASE_OK;
    if (!client_.daemon_alive()) return ASE_HWMalfunction;
    running_ = true;
    client_.clear_pending_tx();  // nothing from before the last stop may replay
    client_.set_tx_active(true);
    thread_ = std::thread([this] { run(); });
    return ASE_OK;
  }

  ASIOError stop() override {
    running_ = false;
    if (thread_.joinable()) {
      // A host may handle our reset/resync message synchronously and call
      // stop() from the buffer thread itself; it ends on its own then.
      if (std::this_thread::get_id() == thread_.get_id()) thread_.detach();
      else thread_.join();
    }
    if (client_.is_open()) client_.set_tx_active(false);
    return ASE_OK;
  }

  ASIOError getChannels(long* in, long* out) override {
    if (!client_.is_open()) return ASE_NotPresent;
    *in = in_ch_;
    *out = out_ch_;
    return ASE_OK;
  }

  ASIOError getLatencies(long* in, long* out) override {
    if (!client_.is_open()) return ASE_NotPresent;
    const long b = long(buffer_frames_ ? buffer_frames_ : kPreferred);
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
    if (!client_.is_open()) return ASE_NotPresent;
    return std::fabs(rate - double(client_.sample_rate())) < 0.5 ? ASE_OK : ASE_NoClock;
  }
  ASIOError getSampleRate(ASIOSampleRate* rate) override {
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
    if (*num < 1) return ASE_InvalidParameter;
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

  ASIOError getSamplePosition(ASIOSamples* pos, ASIOTimeStamp* ts) override {
    if (!running_) return ASE_SPNotAdvancing;
    to_asio64(position_.load(), &pos->hi, &pos->lo);
    to_asio64(switch_ns_.load(), &ts->hi, &ts->lo);
    return ASE_OK;
  }

  ASIOError getChannelInfo(ASIOChannelInfo* info) override {
    const bool in = info->isInput != ASIOFalse;
    if (info->channel < 0 || info->channel >= (in ? in_ch_ : out_ch_)) return ASE_InvalidParameter;
    info->type = ASIOSTInt32LSB;
    info->channelGroup = 0;
    info->isActive = ASIOFalse;
    for (const auto& c : (in ? inputs_ : outputs_))
      if (c.channel == info->channel) info->isActive = ASIOTrue;
    std::snprintf(info->name, sizeof info->name, "Virgil %s %ld", in ? "In" : "Out", info->channel + 1);
    return ASE_OK;
  }

  ASIOError createBuffers(ASIOBufferInfo* infos, long n, long size, ASIOCallbacks* cb) override {
    if (!client_.is_open()) return ASE_NotPresent;
    if (size < kMin || size > kMax || !cb) return ASE_InvalidMode;
    if (size > long(client_.ring_frames() / 4)) return ASE_InvalidMode;
    disposeBuffers();
    for (long i = 0; i < n; ++i) {
      ASIOBufferInfo& bi = infos[i];
      const bool in = bi.isInput != ASIOFalse;
      if (bi.channelNum < 0 || bi.channelNum >= (in ? in_ch_ : out_ch_)) {
        disposeBuffers();
        return ASE_InvalidParameter;
      }
      Channel c;
      c.channel = bi.channelNum;
      c.data.assign(size_t(size) * 2, 0);
      (in ? inputs_ : outputs_).push_back(std::move(c));
    }
    // Hand out the double-buffer halves once all channels are allocated.
    size_t ii = 0, oo = 0;
    for (long i = 0; i < n; ++i) {
      Channel& c = infos[i].isInput != ASIOFalse ? inputs_[ii++] : outputs_[oo++];
      infos[i].buffers[0] = c.data.data();
      infos[i].buffers[1] = c.data.data() + size;
    }
    buffer_frames_ = uint32_t(size);
    callbacks_ = cb;
    time_info_ = cb->asioMessage &&
                 cb->asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) == 1 &&
                 cb->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
    return ASE_OK;
  }

  ASIOError disposeBuffers() override {
    stop();
    inputs_.clear();
    outputs_.clear();
    callbacks_ = nullptr;
    buffer_frames_ = 0;
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
  struct Channel {
    long channel = 0;
    std::vector<int32_t> data;  // two halves of buffer_frames_ each
  };

  static constexpr long kMin = 32;
  static constexpr long kMax = 2048;
  static constexpr long kPreferred = 128;
  static constexpr long kGranularity = 16;

  void set_error(const char* s) {
    std::strncpy(error_, s, sizeof error_ - 1);
    error_[sizeof error_ - 1] = 0;
  }

  // Buffer-switch thread: wakes on media-clock buffer boundaries.
  void run() {
    const uint32_t B = buffer_frames_;
    virgil::set_realtime_priority(0, int64_t(B) * 1000000000LL / client_.sample_rate());
    const uint32_t rx_lat = client_.rx_latency_frames();
    const uint32_t out_off = B + client_.tx_lead_frames();
    const uint32_t rxch = client_.rx_channels(), txch = client_.tx_channels();

    double now = 0;
    client_.frame_now(&now);
    uint64_t t = (uint64_t(now) / B + 1) * B;  // first boundary
    uint64_t k = 0;
    ASIOTime time_info{};
    uint64_t steps = client_.header()->clock_steps.load();
    bool reset_requested = false;
    // How far the next callback may sit from "now" before we realign: a
    // couple of buffers ahead, a quarter ring behind.
    const uint64_t max_ahead = 2 * uint64_t(B) + client_.sample_rate() / 20;
    const uint64_t max_behind = client_.ring_frames() / 4;
    const int64_t max_sleep_ns = 20000000;

    while (running_) {
      // virgild gone for >2 s (settings changed the soundcard layout, or it was
      // restarted): ask the host to re-initialise us so init() reconnects.
      // Shorter gaps (settings applied with the same layout) ride through on
      // the extrapolated clock.
      if (!reset_requested && !client_.heartbeat_fresh(2000000000LL) && callbacks_->asioMessage &&
          callbacks_->asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr)) {
        callbacks_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        reset_requested = true;
      }
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
      const uint64_t cur_steps = client_.header()->clock_steps.load(std::memory_order_relaxed);
      // Realign when the timeline moved (PTP relock, engine restarted with a
      // new clock) or when we are far off it for any other reason: a
      // callback that is due years from now, or one so late that the rings
      // have wrapped, would otherwise silence playback until the host
      // restarts the driver.
      if (cur_steps != steps || double(t) > now_frame + double(max_ahead) ||
          double(t) + double(max_behind) < now_frame) {
        steps = cur_steps;
        t = (uint64_t(now_frame) / B + 1) * B;
        if (callbacks_->asioMessage) callbacks_->asioMessage(kAsioResyncRequest, 0, nullptr, nullptr);
      }
      const int64_t deadline = a.host_ns_at(double(t));
      if (deadline - now_ns > max_sleep_ns) {
        // Never commit to a long sleep: the anchor may change under us.
        virgil::sleep_until_ns(now_ns + max_sleep_ns);
        continue;
      }
      virgil::sleep_until_ns(deadline, 50000);
      if (!running_) break;

      const long idx = long(k & 1);
      // Capture: the B frames ending rx_lat before now.
      const uint64_t in_start = t - B - rx_lat;
      for (auto& c : inputs_) {
        int32_t* dst = c.data.data() + size_t(idx) * B;
        if (uint32_t(c.channel) >= rxch) continue;
        for (uint32_t f = 0; f < B; ++f)
          dst[f] = virgil::float_to_s32(client_.rx_frame(in_start + f)[c.channel]);
      }

      position_ = int64_t(k) * B;
      switch_ns_ = deadline;
      if (time_info_) {
        std::memset(&time_info, 0, sizeof time_info);
        AsioTimeInfo& ti = time_info.timeInfo;
        ti.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
        ti.speed = 1.0;
        ti.sampleRate = client_.sample_rate();
        to_asio64(int64_t(k) * B, &ti.samplePosition.hi, &ti.samplePosition.lo);
        to_asio64(deadline, &ti.systemTime.hi, &ti.systemTime.lo);
        callbacks_->bufferSwitchTimeInfo(&time_info, idx, ASIOTrue);
      } else {
        callbacks_->bufferSwitch(idx, ASIOTrue);
      }

      // Playback: the host filled half `idx`; it plays one buffer later.
      // Frames whose slot the daemon already consumed (we woke very late)
      // are dropped rather than left to replay a ring later.
      const uint64_t out_start = t + out_off;
      const uint64_t horizon = client_.tx_horizon();
      for (uint32_t f = 0; f < B; ++f) {
        if (out_start + f < horizon) continue;
        float* dst = client_.tx_frame(out_start + f);
        for (uint32_t c = 0; c < txch; ++c) dst[c] = 0.f;
        for (const auto& c : outputs_)
          if (uint32_t(c.channel) < txch)
            dst[c.channel] = virgil::s32_to_float(c.data[size_t(idx) * B + f]);
      }
      client_.touch();
      t += B;
      ++k;
    }
  }

  std::atomic<long> refs_{1};
  virgil::Client client_;
  char error_[128] = "";
  long in_ch_ = 0, out_ch_ = 0;
  std::vector<Channel> inputs_, outputs_;
  uint32_t buffer_frames_ = 0;
  ASIOCallbacks* callbacks_ = nullptr;
  bool time_info_ = false;
  std::atomic<bool> running_{false};
  std::thread thread_;
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

STDAPI DllCanUnloadNow() { return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE; }

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
