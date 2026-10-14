#include "asio_host.h"

#include <objbase.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <mutex>

#include "asio_convert.h"
#include "virgil/platform.h"

#ifdef VIRGIL_HAVE_ASIO_SDK
#include <mmsystem.h>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#endif

namespace ah {

const wchar_t* const kNotBuiltIn =
    L"ASIO support was not built in (this Virgil Control was built without the ASIO SDK): use WASAPI";

namespace {

// Virgil's own ASIO driver (drivers/asio): hosting it here would loop
// Virgil into itself.
const wchar_t kVirgilClsid[] = L"{ACDDF2EF-FA26-401C-9CA6-BA447CEA8D38}";

bool same_name(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

std::wstring reg_string(HKEY k, const wchar_t* value) {
  wchar_t buf[512];
  DWORD type = 0, n = sizeof buf - sizeof(wchar_t);
  if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &n) != ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ))
    return std::wstring();
  buf[n / sizeof(wchar_t)] = 0;
  return buf;
}

// HKLM\SOFTWARE\ASIO in one registry view: name -> (clsid, description).
std::vector<Driver> read_asio_key(REGSAM view) {
  std::vector<Driver> r;
  HKEY root = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | view, &root) != ERROR_SUCCESS) return r;
  for (DWORD i = 0;; ++i) {
    wchar_t name[256];
    DWORD n = 256;
    if (RegEnumKeyExW(root, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, name, 0, KEY_READ | view, &k) != ERROR_SUCCESS) continue;
    Driver d;
    d.name = name;
    d.clsid = reg_string(k, L"CLSID");
    d.description = reg_string(k, L"Description");
    RegCloseKey(k);
    if (!d.clsid.empty() && !same_name(d.clsid, kVirgilClsid)) r.push_back(d);
  }
  RegCloseKey(root);
  return r;
}

// The COM class is registered for this process's bitness.
bool loadable(const std::wstring& clsid) {
  const std::wstring path = L"CLSID\\" + clsid + L"\\InprocServer32";
  HKEY k = nullptr;
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
  RegCloseKey(k);
  return true;
}

}  // namespace

DriverList list_drivers() {
  DriverList out;
  for (Driver& d : read_asio_key(0)) {
    if (loadable(d.clsid)) out.drivers.push_back(d);
    else out.other_bitness.push_back(d.name);
  }
  const REGSAM other = sizeof(void*) == 8 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
  for (Driver& d : read_asio_key(other)) {
    const bool mine = std::any_of(out.drivers.begin(), out.drivers.end(),
                                  [&](const Driver& m) { return same_name(m.name, d.name); });
    const bool listed = std::any_of(out.other_bitness.begin(), out.other_bitness.end(),
                                    [&](const std::wstring& m) { return same_name(m, d.name); });
    if (!mine && !listed) out.other_bitness.push_back(d.name);
  }
  std::sort(out.drivers.begin(), out.drivers.end(),
            [](const Driver& a, const Driver& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
  return out;
}

#ifndef VIRGIL_HAVE_ASIO_SDK

bool built_in() { return false; }
Probe probe(const std::wstring&) {
  Probe p;
  p.error = kNotBuiltIn;
  return p;
}
bool Host::open(const Config&, Callback*, std::wstring* error) {
  if (error) *error = kNotBuiltIn;
  return false;
}
void Host::close() {}
void Host::on_buffer(long, const void*) {}
void Host::thread_main() {}

#else  // VIRGIL_HAVE_ASIO_SDK

namespace {

std::atomic<Host*> g_active{nullptr};  // the one host ASIO's callbacks reach
std::mutex g_driver_mu;                // one driver load / setup at a time (host or probe)
std::mutex g_cache_mu;                 // guards the two below
std::wstring g_cache_name;             // the hosted driver, "" if none
Probe g_cache;

std::wstring widen_ansi(const char* s) {
  // ASIO strings are in the system code page.
  wchar_t w[256];
  const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, w, 256);
  return n > 0 ? std::wstring(w) : std::wstring();
}

std::wstring hr_hex(HRESULT hr) {
  wchar_t b[16];
  _snwprintf(b, 15, L"0x%08lX", static_cast<unsigned long>(hr));
  b[15] = 0;
  return b;
}

std::wstring driver_message(IASIO* d) {
  char msg[128 + 8] = {};
  d->getErrorMessage(msg);
  msg[127] = 0;
  return widen_ansi(msg);
}

// ---- the C callbacks (no user pointer: g_active) ----
void cb_switch(long index, ASIOBool) {
  if (Host* h = g_active.load(std::memory_order_acquire)) h->on_buffer(index, nullptr);
}
ASIOTime* cb_switch_ti(ASIOTime* t, long index, ASIOBool) {
  if (Host* h = g_active.load(std::memory_order_acquire)) h->on_buffer(index, t);
  return t;
}
void cb_rate(ASIOSampleRate) {
  if (Host* h = g_active.load(std::memory_order_acquire)) h->on_reset();
}
long cb_message(long selector, long value, void*, double*) {
  Host* h = g_active.load(std::memory_order_acquire);
  switch (selector) {
    case kAsioSelectorSupported:
      return value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest ||
             value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo;
    case kAsioEngineVersion: return 2;
    case kAsioResetRequest:  // e.g. the user changed the buffer size in the driver's panel
      if (h) h->on_reset();
      return 1;
    case kAsioResyncRequest:  // samples lost: the bridge follows by itself
      if (h) h->on_resync();
      return 1;
    case kAsioLatenciesChanged: return 1;  // nothing depends on the reported latencies
    case kAsioSupportsTimeInfo: return 1;
    default: return 0;
  }
}
ASIOCallbacks g_callbacks = {cb_switch, cb_rate, cb_message, cb_switch_ti};

// A hidden window for init(sysHandle), owned by the thread that calls the driver.
HWND make_window() {
  static const ATOM cls = [] {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VirgilAsioHost";
    return RegisterClassW(&wc);
  }();
  (void)cls;
  return CreateWindowExW(0, L"VirgilAsioHost", L"Virgil ASIO host", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                         GetModuleHandleW(nullptr), nullptr);
}

// Creates and init()s the driver. On failure *error says why.
IASIO* load_driver(const std::wstring& name, HWND wnd, std::wstring* error) {
  const DriverList list = list_drivers();
  const Driver* drv = nullptr;
  for (const Driver& d : list.drivers)
    if (same_name(d.name, name)) drv = &d;
  if (!drv) {
    const bool other = std::any_of(list.other_bitness.begin(), list.other_bitness.end(),
                                   [&](const std::wstring& n) { return same_name(n, name); });
    *error = other ? L"the ASIO driver “" + name + L"” is installed for " +
                         (sizeof(void*) == 8 ? L"32-bit" : L"64-bit") + L" programs only"
                   : L"the ASIO driver “" + name + L"” is not installed";
    return nullptr;
  }
  CLSID clsid;
  if (FAILED(CLSIDFromString(const_cast<wchar_t*>(drv->clsid.c_str()), &clsid))) {
    *error = L"the ASIO driver's registration is broken (CLSID " + drv->clsid + L")";
    return nullptr;
  }
  IASIO* d = nullptr;
  // ASIO drivers take their own CLSID as the interface id.
  const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&d));
  if (FAILED(hr) || !d) {
    *error = L"cannot load the ASIO driver (" + hr_hex(hr) + L")";
    return nullptr;
  }
  if (!d->init(wnd)) {
    const std::wstring msg = driver_message(d);
    d->Release();
    *error = L"The ASIO driver is in use by another app (or cannot open): close it, or use WASAPI" +
             (msg.empty() ? std::wstring() : L" (" + msg + L")");
    return nullptr;
  }
  return d;
}

void read_details(IASIO* d, Probe* p) {
  long ni = 0, no = 0;
  if (d->getChannels(&ni, &no) != ASE_OK) ni = no = 0;
  p->inputs = unsigned(std::max(0L, ni));
  p->outputs = unsigned(std::max(0L, no));
  for (int dir = 0; dir < 2; ++dir) {
    const long n = dir == 0 ? ni : no;
    auto& names = dir == 0 ? p->input_names : p->output_names;
    names.clear();
    for (long c = 0; c < n && c < 256; ++c) {
      ASIOChannelInfo ci{};
      ci.channel = c;
      ci.isInput = dir == 0 ? ASIOTrue : ASIOFalse;
      ci.name[0] = 0;
      if (d->getChannelInfo(&ci) == ASE_OK) {
        ci.name[sizeof ci.name - 1] = 0;
        names.push_back(widen_ansi(ci.name));
      } else {
        names.push_back(std::wstring());
      }
    }
  }
  long mn = 0, mx = 0, pr = 0, gr = 0;
  if (d->getBufferSize(&mn, &mx, &pr, &gr) == ASE_OK) {
    p->min_buffer = mn;
    p->max_buffer = mx;
    p->pref_buffer = pr;
    p->granularity = gr;
  }
  ASIOSampleRate sr = 0;
  if (d->getSampleRate(&sr) == ASE_OK) p->rate = sr;
}

}  // namespace

bool built_in() { return true; }

Probe probe(const std::wstring& driver) {
  {
    std::lock_guard<std::mutex> l(g_cache_mu);
    if (!g_cache_name.empty() && same_name(g_cache_name, driver)) return g_cache;
  }
  Probe p;
  std::thread([&] {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND wnd = make_window();
    {
      std::lock_guard<std::mutex> l(g_driver_mu);
      bool cached = false;
      {
        std::lock_guard<std::mutex> c(g_cache_mu);  // hosted meanwhile?
        if (!g_cache_name.empty() && same_name(g_cache_name, driver)) {
          p = g_cache;
          cached = true;
        }
      }
      if (!cached) {
        if (IASIO* d = load_driver(driver, wnd, &p.error)) {
          read_details(d, &p);
          p.ok = true;
          d->Release();
        }
      }
    }
    if (wnd) DestroyWindow(wnd);
    if (SUCCEEDED(co)) CoUninitialize();
  }).join();
  return p;
}

bool Host::open(const Config& cfg, Callback* cb, std::wstring* error) {
  close();
  Host* expected = nullptr;
  if (!g_active.compare_exchange_strong(expected, this)) {
    if (error) *error = L"only one ASIO driver at a time";
    return false;
  }
  cfg_ = cfg;
  cb_ = cb;
  ok_ = false;
  error_.clear();
  note_.clear();
  reset_.store(false);
  switches_.store(0);
  resyncs_.store(0);
  tid_.store(0);
  ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  th_ = std::thread([this] { thread_main(); });
  // Drivers can take a few seconds (firmware, USB enumeration).
  const bool answered = WaitForSingleObject(ready_, 15000) == WAIT_OBJECT_0;
  if (!answered || !ok_) {
    const std::wstring e = answered ? error_ : L"the ASIO driver did not answer in time";
    close();
    if (error) *error = e;
    return false;
  }
  return true;
}

void Host::close() {
  if (th_.joinable()) {
    // The host thread made its message queue before it signalled anything;
    // wait for its id (set first thing), then end its loop.
    for (int i = 0; i < 1000 && !tid_.load(); ++i) Sleep(1);
    for (int i = 0; i < 100 && !PostThreadMessageW(tid_.load(), WM_QUIT, 0, 0); ++i) Sleep(5);
    th_.join();
  }
  if (ready_) CloseHandle(ready_);
  ready_ = nullptr;
  Host* me = this;
  g_active.compare_exchange_strong(me, nullptr);
}

void Host::on_buffer(long index, const void* time_info) {
  if (!armed_.load(std::memory_order_acquire) || index < 0 || index > 1) return;
  const int64_t now = virgil::mono_ns();
  int64_t end_ns = now;
  if (time_info) {
    // systemTime: when this buffer half completed, in ns on timeGetTime()'s
    // clock. Used for its offset from now when plausible (it removes the
    // callback's scheduling delay), else the time of the callback.
    const ASIOTime* t = static_cast<const ASIOTime*>(time_info);
    if (t->timeInfo.flags & kSystemTimeValid) {
      const uint64_t st = uint64_t(t->timeInfo.systemTime.hi) << 32 | uint64_t(t->timeInfo.systemTime.lo);
      const int64_t st_ms = int64_t(st / 1000000), st_rem = int64_t(st % 1000000);
      const int32_t dms = int32_t(uint32_t(timeGetTime()) - uint32_t(st_ms));  // wraps every 49 days
      const int64_t delta = int64_t(dms) * 1000000 - st_rem;
      const int64_t limit = int64_t(2.0 * frames_ / rate_ * 1e9) + 2000000;
      if (delta >= 0 && delta <= limit) end_ns = now - delta;
    }
  }
  const size_t n = frames_;
  for (unsigned k = 0; k < nin_; ++k) asioconv::to_float(types_[k], bufs_[index][k], bytes_[k], n, in_.data() + k, nin_);
  cb_->process(nin_ ? in_.data() : nullptr, nout_ ? out_.data() : nullptr, n, end_ns);
  for (unsigned k = 0; k < nout_; ++k)
    asioconv::from_float(types_[nin_ + k], out_.data() + k, nout_, n, bufs_[index][nin_ + k], bytes_[nin_ + k]);
  if (output_ready_) static_cast<IASIO*>(drv_)->outputReady();
  switches_.fetch_add(1, std::memory_order_relaxed);
}

void Host::thread_main() {
  MSG msg;
  PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the message queue, for PostThreadMessage
  tid_.store(GetCurrentThreadId());
  // Apartment-threaded, as ASIO drivers expect of the thread that calls them.
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  HWND wnd = make_window();
  IASIO* d = nullptr;
  bool buffers = false, started = false;
  std::wstring err;
  {
    std::lock_guard<std::mutex> l(g_driver_mu);
    d = load_driver(cfg_.driver, wnd, &err);
    drv_ = d;
    Probe info;
    if (d) {
      read_details(d, &info);
      info.ok = true;
      dev_in_ = info.inputs;
      dev_out_ = info.outputs;
      const asioconv::Range ri = asioconv::select_channels(cfg_.first_input, cfg_.inputs, dev_in_);
      const asioconv::Range ro = asioconv::select_channels(cfg_.first_output, cfg_.outputs, dev_out_);
      in_clamped_ = ri.clamped;
      out_clamped_ = ro.clamped;
      nin_ = ri.count;
      nout_ = ro.count;
      wchar_t b[160];
      if (cfg_.inputs && !nin_) {
        _snwprintf(b, 159, L"the interface has %u input%ls: choose others", dev_in_, dev_in_ == 1 ? L"" : L"s");
        b[159] = 0;
        err = b;
      } else if (cfg_.outputs && !nout_) {
        _snwprintf(b, 159, L"the interface has %u output%ls: choose others", dev_out_, dev_out_ == 1 ? L"" : L"s");
        b[159] = 0;
        err = b;
      } else if (!nin_ && !nout_) {
        err = L"no inputs or outputs chosen";
      }
      // Buffer size: the one asked for if the driver allows it.
      long size = info.pref_buffer;
      if (cfg_.buffer) {
        if (asioconv::buffer_allowed(long(cfg_.buffer), info.min_buffer, info.max_buffer, info.pref_buffer,
                                     info.granularity)) {
          size = long(cfg_.buffer);
        } else {
          _snwprintf(b, 159, L"buffer %u not supported by the driver: %ld used", cfg_.buffer, info.pref_buffer);
          b[159] = 0;
          note_ = b;
        }
      }
      if (err.empty() && size <= 0) err = L"the ASIO driver reports no buffer size";
      // Virgil's rate if the interface can run at it (no resampling), else its own.
      ASIOSampleRate sr = info.rate;
      if (err.empty() && sr != cfg_.rate && d->canSampleRate(cfg_.rate) == ASE_OK && d->setSampleRate(cfg_.rate) == ASE_OK)
        sr = cfg_.rate;
      ASIOSampleRate now_rate = 0;
      if (d->getSampleRate(&now_rate) == ASE_OK && now_rate > 0) sr = now_rate;
      if (err.empty() && !(sr >= 8000 && sr <= 768000)) err = L"the ASIO driver reports no sample rate";
      rate_ = sr;
      info.rate = sr;
      // Sample types of the channels to open.
      const unsigned n = nin_ + nout_;
      types_.assign(n, 0);
      bytes_.assign(n, 0);
      for (unsigned k = 0; err.empty() && k < n; ++k) {
        ASIOChannelInfo ci{};
        ci.isInput = k < nin_ ? ASIOTrue : ASIOFalse;
        ci.channel = long(k < nin_ ? ri.first0 + k : ro.first0 + (k - nin_));
        if (d->getChannelInfo(&ci) != ASE_OK) {
          err = L"the ASIO driver does not describe its channels";
          break;
        }
        types_[k] = ci.type;
        bytes_[k] = asioconv::sample_bytes(ci.type);
        if (!bytes_[k]) {
          err = L"the interface's sample format (ASIO type " + std::to_wstring(ci.type) + L") is not supported";
          break;
        }
      }
      if (err.empty()) {
        std::vector<ASIOBufferInfo> bi(n);
        for (unsigned k = 0; k < n; ++k) {
          bi[k].isInput = k < nin_ ? ASIOTrue : ASIOFalse;
          bi[k].channelNum = long(k < nin_ ? ri.first0 + k : ro.first0 + (k - nin_));
          bi[k].buffers[0] = bi[k].buffers[1] = nullptr;
        }
        const ASIOError e = d->createBuffers(bi.data(), long(n), size, &g_callbacks);
        if (e != ASE_OK) {
          const std::wstring m = driver_message(d);
          err = L"The ASIO driver is in use by another app (or cannot open): close it, or use WASAPI" +
                (m.empty() ? std::wstring() : L" (" + m + L")");
        } else {
          buffers = true;
          frames_ = unsigned(size);
          for (int h = 0; h < 2; ++h) {
            bufs_[h].assign(n, nullptr);
            for (unsigned k = 0; k < n; ++k) bufs_[h][k] = bi[k].buffers[h];
          }
          for (unsigned k = 0; k < n; ++k)
            if (!bi[k].buffers[0] || !bi[k].buffers[1]) err = L"the ASIO driver gave no buffers";
          in_.assign(size_t(frames_) * std::max(1u, nin_), 0.f);
          out_.assign(size_t(frames_) * std::max(1u, nout_), 0.f);
          for (int h = 0; err.empty() && h < 2; ++h)
            for (unsigned k = nin_; k < n; ++k) std::memset(bufs_[h][k], 0, size_t(frames_) * bytes_[k]);
          output_ready_ = d->outputReady() == ASE_OK;
        }
      }
      if (err.empty() && !cb_->prepare(rate_, frames_, nin_, nout_)) err = L"out of memory";
      if (err.empty()) {
        reset_.store(false, std::memory_order_release);
        armed_.store(true, std::memory_order_release);
        const ASIOError e = d->start();
        if (e != ASE_OK) {
          armed_.store(false, std::memory_order_release);
          const std::wstring m = driver_message(d);
          err = L"the ASIO driver does not start" + (m.empty() ? std::wstring() : L" (" + m + L")");
        } else {
          started = true;
        }
      }
    }
    if (started) {
      std::lock_guard<std::mutex> c(g_cache_mu);
      g_cache_name = cfg_.driver;
      g_cache = info;
    }
  }
  ok_ = started;
  error_ = err;
  SetEvent(ready_);  // publishes ok_, error_ and the format
  if (started) {
    // The driver's (and its windows') messages until close() posts WM_QUIT.
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  {
    std::lock_guard<std::mutex> l(g_driver_mu);
    if (started) d->stop();
    armed_.store(false, std::memory_order_release);
    if (buffers) d->disposeBuffers();
    if (d) d->Release();
    drv_ = nullptr;
    std::lock_guard<std::mutex> c(g_cache_mu);
    if (started) g_cache_name.clear();
  }
  if (wnd) DestroyWindow(wnd);
  if (SUCCEEDED(co)) CoUninitialize();
}

#endif  // VIRGIL_HAVE_ASIO_SDK

}  // namespace ah
