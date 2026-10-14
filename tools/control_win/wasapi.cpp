#include "wasapi.h"

#include <audiopolicy.h>
#include <propidl.h>
#include <propsys.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <map>

#include "asio_convert.h"
#include "virgil/platform.h"

namespace wa {

namespace {

// Spelled out instead of taken from the SDK's GUID libraries, which differ
// between MinGW and MSVC.
const CLSID kClsidMMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Process loopback (audioclientactivationparams.h, Windows SDK 10.0.20348+),
// which MinGW does not have. Values as documented by Microsoft; prefixed so
// they cannot clash with a header that does declare them.
const wchar_t kVadProcessLoopback[] = L"VAD\\Process_Loopback";  // VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
enum VgActivationType { kActivationDefault = 0, kActivationProcessLoopback = 1 };
enum VgLoopbackMode { kIncludeTargetProcessTree = 0, kExcludeTargetProcessTree = 1 };
struct VgProcessLoopbackParams {  // AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS
  DWORD TargetProcessId;
  VgLoopbackMode ProcessLoopbackMode;
};
struct VgActivationParams {  // AUDIOCLIENT_ACTIVATION_PARAMS
  VgActivationType ActivationType;
  union {
    VgProcessLoopbackParams ProcessLoopbackParams;
  };
};
static_assert(sizeof(VgActivationParams) == 12, "AUDIOCLIENT_ACTIVATION_PARAMS layout");

std::wstring lower(std::wstring s) {
  for (wchar_t& c : s) c = wchar_t(towlower(c));
  return s;
}

// IActivateAudioInterfaceCompletionHandler. Must be agile (IAgileObject):
// Windows calls it on one of its own threads. The waiting thread and the
// completion each hold a reference, so either may finish first.
class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
 public:
  ActivateHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  HANDLE done() const { return done_; }
  // After done() is signalled.
  HRESULT result(IUnknown** out) {
    *out = result_;
    if (result_) result_->AddRef();
    return hr_;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IActivateAudioInterfaceCompletionHandler))) {
      *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
    } else if (IsEqualIID(riid, __uuidof(IAgileObject))) {
      *ppv = static_cast<IAgileObject*>(this);
    } else {
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
    HRESULT activate_hr = E_FAIL;
    IUnknown* unk = nullptr;
    const HRESULT hr = op ? op->GetActivateResult(&activate_hr, &unk) : E_POINTER;
    hr_ = FAILED(hr) ? hr : activate_hr;
    if (SUCCEEDED(hr_) && unk) {
      result_ = unk;
    } else {
      if (unk) unk->Release();
      if (SUCCEEDED(hr_)) hr_ = E_NOINTERFACE;
    }
    SetEvent(done_);  // publishes hr_ and result_ to the waiter
    return S_OK;
  }

 private:
  ~ActivateHandler() {
    if (result_) result_->Release();
    if (done_) CloseHandle(done_);
  }
  std::atomic<ULONG> refs_{1};
  HANDLE done_;
  HRESULT hr_ = E_PENDING;
  IUnknown* result_ = nullptr;
};

using ActivateFn = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
                                     IActivateAudioInterfaceAsyncOperation**);
ActivateFn activate_fn() {
  // Looked up at run time: Mmdevapi.dll has it from Windows 8 on.
  static const ActivateFn fn = [] {
    HMODULE m = LoadLibraryExW(L"Mmdevapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? reinterpret_cast<ActivateFn>(reinterpret_cast<void*>(GetProcAddress(m, "ActivateAudioInterfaceAsync")))
             : nullptr;
  }();
  return fn;
}

float clampf(float x) { return x > 1.f ? 1.f : x < -1.f ? -1.f : x; }

}  // namespace

const wchar_t* const kNeedsWin2004 = L"needs Windows 10 version 2004 or later";

// ---- endpoints, sessions, processes ----------------------------------------------

HRESULT create_enumerator(IMMDeviceEnumerator** out) {
  return CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(out));
}

std::wstring endpoint_id(IMMDevice* d) {
  LPWSTR id = nullptr;
  std::wstring r;
  if (d && SUCCEEDED(d->GetId(&id)) && id) r = id;
  if (id) CoTaskMemFree(id);
  return r;
}

std::wstring endpoint_name(IMMDevice* d) {
  std::wstring r;
  Com<IPropertyStore> ps;
  if (d && SUCCEEDED(d->OpenPropertyStore(STGM_READ, ps.put()))) {
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) r = v.pwszVal;
    PropVariantClear(&v);
  }
  return r;
}

std::wstring default_render_id() {
  Com<IMMDeviceEnumerator> en;
  Com<IMMDevice> d;
  if (FAILED(create_enumerator(en.put())) || FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, d.put())))
    return std::wstring();
  return endpoint_id(d.get());
}

namespace {
std::vector<Endpoint> list_endpoints(EDataFlow flow, HRESULT* out_hr) {
  std::vector<Endpoint> r;
  Com<IMMDeviceEnumerator> en;
  Com<IMMDeviceCollection> col;
  HRESULT hr = create_enumerator(en.put());
  if (SUCCEEDED(hr)) hr = en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, col.put());
  UINT n = 0;
  if (SUCCEEDED(hr)) hr = col->GetCount(&n);
  std::wstring def;
  if (SUCCEEDED(hr)) {
    Com<IMMDevice> d;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, d.put()))) def = endpoint_id(d.get());
  }
  for (UINT i = 0; SUCCEEDED(hr) && i < n; ++i) {
    Com<IMMDevice> d;
    if (FAILED(col->Item(i, d.put()))) continue;
    Endpoint e;
    e.id = endpoint_id(d.get());
    e.name = endpoint_name(d.get());
    if (e.name.empty()) e.name = e.id;
    e.is_default = !def.empty() && e.id == def;
    r.push_back(e);
  }
  if (out_hr) *out_hr = hr;
  return r;
}

HRESULT open_endpoint(EDataFlow flow, const std::wstring& id, IMMDevice** out) {
  Com<IMMDeviceEnumerator> en;
  HRESULT hr = create_enumerator(en.put());
  if (FAILED(hr)) return hr;
  if (id.empty() || id == L"default") return en->GetDefaultAudioEndpoint(flow, eConsole, out);
  Com<IMMDevice> d;
  hr = en->GetDevice(id.c_str(), d.put());
  if (FAILED(hr)) return hr;
  DWORD state = 0;
  if (SUCCEEDED(d->GetState(&state)) && state != DEVICE_STATE_ACTIVE)
    return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
  *out = d.get();
  (*out)->AddRef();
  return S_OK;
}
}  // namespace

std::vector<Endpoint> render_endpoints(HRESULT* hr) { return list_endpoints(eRender, hr); }
std::vector<Endpoint> capture_endpoints(HRESULT* hr) { return list_endpoints(eCapture, hr); }
HRESULT open_render_endpoint(const std::wstring& id, IMMDevice** out) { return open_endpoint(eRender, id, out); }
HRESULT open_capture_endpoint(const std::wstring& id, IMMDevice** out) { return open_endpoint(eCapture, id, out); }

std::wstring default_capture_id() {
  Com<IMMDeviceEnumerator> en;
  Com<IMMDevice> d;
  if (FAILED(create_enumerator(en.put())) || FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, d.put())))
    return std::wstring();
  return endpoint_id(d.get());
}

unsigned endpoint_channels(bool capture, const std::wstring& id, unsigned* rate, std::wstring* error) {
  Com<IMMDevice> dev;
  Com<IAudioClient> client;
  WAVEFORMATEX* mix = nullptr;
  HRESULT hr = open_endpoint(capture ? eCapture : eRender, id, dev.put());
  if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void());
  if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
  unsigned ch = 0;
  if (SUCCEEDED(hr) && mix) {
    ch = mix->nChannels;
    if (rate) *rate = mix->nSamplesPerSec;
  } else if (error) {
    *error = hr_text(hr);
  }
  if (mix) CoTaskMemFree(mix);
  return ch;
}

std::wstring exe_name_of(DWORD pid) {
  if (pid == 0) return std::wstring();
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return std::wstring();
  wchar_t path[MAX_PATH * 2];
  DWORD n = DWORD(sizeof path / sizeof path[0]);
  std::wstring r;
  if (QueryFullProcessImageNameW(h, 0, path, &n)) {
    r.assign(path, n);
    const size_t slash = r.find_last_of(L"\\/");
    if (slash != std::wstring::npos) r = r.substr(slash + 1);
  }
  CloseHandle(h);
  return r;
}

std::vector<AudioApp> audio_apps() {
  std::map<std::wstring, AudioApp> by_exe;  // lower-case exe -> entry
  Com<IMMDeviceEnumerator> en;
  Com<IMMDeviceCollection> col;
  UINT n = 0;
  if (FAILED(create_enumerator(en.put())) || FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.put())) ||
      FAILED(col->GetCount(&n)))
    return {};
  const DWORD self = GetCurrentProcessId();
  for (UINT i = 0; i < n; ++i) {
    Com<IMMDevice> d;
    Com<IAudioSessionManager2> mgr;
    Com<IAudioSessionEnumerator> sessions;
    if (FAILED(col->Item(i, d.put())) ||
        FAILED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, mgr.put_void())) ||
        FAILED(mgr->GetSessionEnumerator(sessions.put())))
      continue;
    int count = 0;
    if (FAILED(sessions->GetCount(&count))) continue;
    for (int k = 0; k < count; ++k) {
      Com<IAudioSessionControl> ctl;
      Com<IAudioSessionControl2> ctl2;
      if (FAILED(sessions->GetSession(k, ctl.put())) ||
          FAILED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), ctl2.put_void())))
        continue;
      if (ctl2->IsSystemSoundsSession() == S_OK) continue;
      DWORD pid = 0;
      if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0 || pid == self) continue;
      AudioSessionState st = AudioSessionStateInactive;
      ctl->GetState(&st);
      if (st == AudioSessionStateExpired) continue;
      const std::wstring exe = exe_name_of(pid);
      if (exe.empty()) continue;
      AudioApp& a = by_exe[lower(exe)];
      if (a.exe.empty()) {
        a.exe = exe;
        a.pid = pid;
      }
      LPWSTR name = nullptr;
      if (a.display.empty() && SUCCEEDED(ctl->GetDisplayName(&name)) && name && *name && name[0] != L'@')
        a.display = name;
      if (name) CoTaskMemFree(name);
      a.active |= st == AudioSessionStateActive;
    }
  }
  std::vector<AudioApp> r;
  for (auto& kv : by_exe) r.push_back(kv.second);
  // Playing first, then by name.
  std::stable_sort(r.begin(), r.end(), [](const AudioApp& a, const AudioApp& b) { return a.active > b.active; });
  return r;
}

std::vector<DWORD> root_processes(const std::wstring& exe) {
  std::vector<DWORD> r;
  if (exe.empty()) return r;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return r;
  const std::wstring want = lower(exe);
  std::map<DWORD, std::pair<DWORD, bool>> procs;  // pid -> (parent, matches)
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof pe;
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    procs[pe.th32ProcessID] = {pe.th32ParentProcessID, lower(pe.szExeFile) == want};
  CloseHandle(snap);
  const DWORD self = GetCurrentProcessId();
  for (const auto& kv : procs) {
    if (!kv.second.second || kv.first == self || kv.first == 0) continue;
    const auto parent = procs.find(kv.second.first);
    const bool parent_matches = parent != procs.end() && parent->second.second && parent->first != kv.first;
    if (!parent_matches) r.push_back(kv.first);
  }
  return r;
}

DWORD windows_build() {
  using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
  static const DWORD build = [] {
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    if (fn && fn(&v) == 0) return v.dwMajorVersion > 10 ? DWORD(99999) : v.dwMajorVersion == 10 ? v.dwBuildNumber : 0;
    return DWORD(0);
  }();
  return build;
}

bool process_loopback_supported() { return windows_build() >= 19041 && activate_fn() != nullptr; }

// ---- formats -----------------------------------------------------------------------

bool describe_format(const WAVEFORMATEX* wf, Format* f) {
  if (!wf || !f) return false;
  *f = Format();
  f->rate = wf->nSamplesPerSec;
  f->channels = wf->nChannels;
  f->bits = wf->wBitsPerSample;
  f->block = wf->nBlockAlign;
  bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT, is_pcm = wf->wFormatTag == WAVE_FORMAT_PCM;
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
    const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    is_float = IsEqualGUID(x->SubFormat, kSubtypeFloat) != 0;
    is_pcm = IsEqualGUID(x->SubFormat, kSubtypePcm) != 0;
    f->mask = x->dwChannelMask;
  }
  f->is_float = is_float;
  if (f->rate < 8000 || f->rate > 768000 || f->channels == 0 || f->channels > 32) return false;
  if (is_float && f->bits != 32 && f->bits != 64) return false;
  if (is_pcm && f->bits != 16 && f->bits != 24 && f->bits != 32) return false;
  if (!is_float && !is_pcm) return false;
  if (f->block != f->channels * (f->bits / 8)) return false;
  f->sample_type = is_float ? (f->bits == 32 ? asioconv::kFloat32LSB : asioconv::kFloat64LSB)
                   : f->bits == 16 ? asioconv::kInt16LSB
                   : f->bits == 24 ? asioconv::kInt24LSB
                                   : asioconv::kInt32LSB;  // 32, or 24 valid bits left-justified

  // Speaker position of each channel: the mask's bits in increasing order.
  const unsigned n = f->channels;
  DWORD pos[32] = {};
  if (f->mask) {
    unsigned c = 0;
    for (DWORD bit = 1; bit && c < n; bit <<= 1)
      if (f->mask & bit) pos[c++] = bit;
  } else if (n == 1) {
    pos[0] = SPEAKER_FRONT_CENTER;
  } else {
    pos[0] = SPEAKER_FRONT_LEFT;
    pos[1] = SPEAKER_FRONT_RIGHT;
  }
  const float h = 0.70710678f;
  for (unsigned c = 0; c < n; ++c) {
    float l = 0, r = 0;
    uint8_t from = 0;
    switch (pos[c]) {
      case SPEAKER_FRONT_LEFT: l = 1; from = 1; break;
      case SPEAKER_FRONT_RIGHT: r = 1; from = 2; break;
      case SPEAKER_FRONT_CENTER: l = r = h; break;
      case SPEAKER_BACK_LEFT: case SPEAKER_SIDE_LEFT: case SPEAKER_FRONT_LEFT_OF_CENTER: l = h; break;
      case SPEAKER_BACK_RIGHT: case SPEAKER_SIDE_RIGHT: case SPEAKER_FRONT_RIGHT_OF_CENTER: r = h; break;
      case SPEAKER_BACK_CENTER: l = r = 0.5f; break;
      case SPEAKER_TOP_FRONT_LEFT: case SPEAKER_TOP_BACK_LEFT: l = 0.5f; break;
      case SPEAKER_TOP_FRONT_RIGHT: case SPEAKER_TOP_BACK_RIGHT: r = 0.5f; break;
      default: break;  // LFE, top centre, unassigned: left out
    }
    f->to_l[c] = l;
    f->to_r[c] = r;
    f->from[c] = from;
  }
  if (n == 1) {  // a mono device: both ways
    f->to_l[0] = f->to_r[0] = 1;
    f->from[0] = 3;
  }
  return true;
}

std::wstring format_text(const Format& f) {
  wchar_t b[96];
  _snwprintf(b, 95, L"%.1f kHz, %u ch, %ls%u", f.rate / 1000.0, f.channels, f.is_float ? L"float" : L"int", f.bits);
  b[95] = 0;
  std::wstring s = b;
  if (s.find(L".0 kHz") != std::wstring::npos) s.erase(s.find(L".0 kHz"), 2);
  return s;
}

WAVEFORMATEXTENSIBLE make_format(unsigned rate, unsigned channels, bool is_float, unsigned bits) {
  WAVEFORMATEXTENSIBLE x{};
  x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  x.Format.nChannels = WORD(channels);
  x.Format.nSamplesPerSec = rate;
  x.Format.wBitsPerSample = WORD(bits);
  x.Format.nBlockAlign = WORD(channels * bits / 8);
  x.Format.nAvgBytesPerSec = rate * x.Format.nBlockAlign;
  x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  x.Samples.wValidBitsPerSample = WORD(bits);
  x.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  x.SubFormat = is_float ? kSubtypeFloat : kSubtypePcm;
  return x;
}

namespace {
inline float sample_at(const BYTE* p, const Format& f) {
  if (f.is_float) {
    if (f.bits == 32) {
      float v;
      std::memcpy(&v, p, 4);
      return v == v ? v : 0.f;  // NaN: silence
    }
    double v;
    std::memcpy(&v, p, 8);
    return v == v ? float(v) : 0.f;
  }
  switch (f.bits) {
    case 16: {
      int16_t v;
      std::memcpy(&v, p, 2);
      return float(v) * (1.f / 32768.f);
    }
    case 24: {
      const int32_t v = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8;
      return float(v) * (1.f / 8388608.f);
    }
    default: {  // 32 bit, or 24 valid bits left-justified in 32
      int32_t v;
      std::memcpy(&v, p, 4);
      return float(v) * (1.f / 2147483648.f);
    }
  }
}

inline void store_sample(float x, BYTE* p, const Format& f) {
  if (f.is_float) {
    if (f.bits == 32) std::memcpy(p, &x, 4);
    else {
      const double d = x;
      std::memcpy(p, &d, 8);
    }
    return;
  }
  const float c = clampf(x == x ? x : 0.f);
  switch (f.bits) {
    case 16: {
      const int16_t v = int16_t(std::lrint(c * 32767.f));
      std::memcpy(p, &v, 2);
      break;
    }
    case 24: {
      const int32_t v = int32_t(std::lrint(c * 8388607.f));
      p[0] = BYTE(v);
      p[1] = BYTE(v >> 8);
      p[2] = BYTE(v >> 16);
      break;
    }
    default: {
      const int32_t v = int32_t(std::lrint(double(c) * 2147483647.0));
      std::memcpy(p, &v, 4);
      break;
    }
  }
}
}  // namespace

void read_frames(const BYTE* src, const Format& f, size_t frames, float* dst, unsigned out_ch) {
  const unsigned n = f.channels, bps = f.bits / 8;
  for (size_t i = 0; i < frames; ++i) {
    const BYTE* p = src + i * f.block;
    float l = 0, r = 0;
    if (n == 2 && f.to_l[0] == 1 && f.to_r[1] == 1) {  // the usual case
      l = sample_at(p, f);
      r = sample_at(p + bps, f);
    } else {
      for (unsigned c = 0; c < n; ++c) {
        const float s = sample_at(p + c * bps, f);
        l += f.to_l[c] * s;
        r += f.to_r[c] * s;
      }
    }
    if (out_ch == 1) dst[i] = 0.5f * (l + r);
    else {
      dst[2 * i] = l;
      dst[2 * i + 1] = r;
    }
  }
}

void write_frames(const float* src, unsigned src_ch, size_t frames, const Format& f, BYTE* dst) {
  const unsigned n = f.channels, bps = f.bits / 8;
  for (size_t i = 0; i < frames; ++i) {
    const float l = src ? src[i * src_ch] : 0.f;
    const float r = src ? (src_ch > 1 ? src[i * src_ch + 1] : l) : 0.f;
    BYTE* p = dst + i * f.block;
    for (unsigned c = 0; c < n; ++c) {
      const uint8_t from = f.from[c];
      store_sample(from == 1 ? l : from == 2 ? r : from == 3 ? 0.5f * (l + r) : 0.f, p + c * bps, f);
    }
  }
}

// ---- errors ------------------------------------------------------------------------

std::wstring hr_text(HRESULT hr) {
  struct Known {
    HRESULT hr;
    const wchar_t* what;
  };
  const Known known[] = {
      {AUDCLNT_E_DEVICE_INVALIDATED, L"the audio device was removed or changed"},
      {AUDCLNT_E_DEVICE_IN_USE, L"the device is in exclusive use by another app"},
      {AUDCLNT_E_UNSUPPORTED_FORMAT, L"the device's format is not supported"},
      {AUDCLNT_E_SERVICE_NOT_RUNNING, L"the Windows Audio service is not running"},
      {AUDCLNT_E_RESOURCES_INVALIDATED, L"the audio resources were invalidated"},
      {E_ACCESSDENIED, L"access denied"},
      {E_NOTIMPL, L"not supported by this version of Windows"},
      {E_OUTOFMEMORY, L"out of memory"},
      {HRESULT_FROM_WIN32(ERROR_NOT_FOUND), L"device not found"},
      {HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED), L"device not connected"},
      {HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Windows did not answer in time"},
      {HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), L"not available on this version of Windows"},
  };
  const wchar_t* what = nullptr;
  for (const Known& k : known)
    if (k.hr == hr) what = k.what;
  std::wstring s;
  if (what) {
    s = what;
  } else {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, DWORD(hr), 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    if (n && buf) {
      s.assign(buf, n);
      while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.' || s.back() == L' ')) s.pop_back();
    }
    if (buf) LocalFree(buf);
    if (s.empty()) s = L"error";
  }
  wchar_t code[16];
  _snwprintf(code, 15, L" (0x%08lX)", static_cast<unsigned long>(hr));
  code[15] = 0;
  return s + code;
}

// ---- process loopback ------------------------------------------------------------

HRESULT activate_process_loopback(DWORD pid, bool include_tree, IAudioClient** out, DWORD timeout_ms) {
  *out = nullptr;
  if (!process_loopback_supported()) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  VgActivationParams params{};
  params.ActivationType = kActivationProcessLoopback;
  params.ProcessLoopbackParams.TargetProcessId = pid;
  params.ProcessLoopbackParams.ProcessLoopbackMode = include_tree ? kIncludeTargetProcessTree : kExcludeTargetProcessTree;
  PROPVARIANT pv;
  PropVariantInit(&pv);
  pv.vt = VT_BLOB;
  pv.blob.cbSize = sizeof params;
  pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

  ActivateHandler* handler = new ActivateHandler();  // our reference
  if (!handler->done()) {
    handler->Release();
    return E_OUTOFMEMORY;
  }
  IActivateAudioInterfaceAsyncOperation* op = nullptr;
  HRESULT hr = activate_fn()(kVadProcessLoopback, __uuidof(IAudioClient), &pv, handler, &op);
  if (SUCCEEDED(hr)) {
    if (WaitForSingleObject(handler->done(), timeout_ms) == WAIT_OBJECT_0) {
      Com<IUnknown> unk;
      hr = handler->result(unk.put());
      if (SUCCEEDED(hr)) hr = unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(out));
    } else {
      hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);  // the handler cleans up if it completes later
    }
  }
  if (op) op->Release();
  handler->Release();
  return hr;
}

// ---- capture ---------------------------------------------------------------------

void Capture::close() {
  if (client_) client_->Stop();
  cap_.reset();
  client_.reset();
  if (event_) CloseHandle(event_);
  event_ = nullptr;
  endpoint_.clear();
}

HRESULT Capture::finish_open(std::wstring* error) {
  HRESULT hr = client_->GetService(__uuidof(IAudioCaptureClient), cap_.put_void());
  if (FAILED(hr)) {
    if (error) *error = L"cannot capture: " + hr_text(hr);
    close();
    return hr;
  }
  // One second of the largest packets the converted stream may need.
  scratch_.reserve(size_t(fmt_.rate) * 2);
  return S_OK;
}

HRESULT Capture::open_device(const std::wstring& endpoint, std::wstring* error) {
  close();
  Com<IMMDevice> dev;
  HRESULT hr = open_render_endpoint(endpoint, dev.put());
  if (FAILED(hr)) {
    if (error) *error = (endpoint.empty() || endpoint == L"default" ? L"no default output device: " : L"output device not available: ") + hr_text(hr);
    return hr;
  }
  endpoint_ = endpoint_id(dev.get());
  // With event notifications if this Windows supports them for loopback
  // (Windows 10 does), else polled.
  for (int attempt = 0; attempt < 2; ++attempt) {
    const bool events = attempt == 0;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.put_void());
    if (FAILED(hr)) break;
    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr)) break;
    if (!describe_format(mix, &fmt_)) {
      CoTaskMemFree(mix);
      hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
      break;
    }
    // 100 ms of buffer: room for a late capture thread.
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK | (events ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0), 1000000, 0,
                             mix, nullptr);
    CoTaskMemFree(mix);
    if (SUCCEEDED(hr) && events) {
      event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      hr = event_ ? client_->SetEventHandle(event_) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) break;
    client_.reset();
    if (event_) CloseHandle(event_);
    event_ = nullptr;
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) break;
  }
  if (FAILED(hr)) {
    if (error) *error = L"cannot capture the output device: " + hr_text(hr);
    const std::wstring keep = endpoint_;
    close();
    endpoint_ = keep;
    return hr;
  }
  return finish_open(error);
}

HRESULT Capture::open_input(const std::wstring& endpoint, std::wstring* error) {
  close();
  Com<IMMDevice> dev;
  HRESULT hr = open_capture_endpoint(endpoint, dev.put());
  if (FAILED(hr)) {
    if (error)
      *error = (endpoint.empty() || endpoint == L"default" ? L"no default recording device: " : L"recording device not available: ") +
               hr_text(hr);
    return hr;
  }
  endpoint_ = endpoint_id(dev.get());
  WAVEFORMATEX* mix = nullptr;
  hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.put_void());
  if (SUCCEEDED(hr)) hr = client_->GetMixFormat(&mix);
  if (SUCCEEDED(hr) && !describe_format(mix, &fmt_)) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
  // Event driven, the shortest buffer shared mode allows (one device period).
  if (SUCCEEDED(hr))
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, 0,
                             0, mix, nullptr);
  if (mix) CoTaskMemFree(mix);
  if (SUCCEEDED(hr)) {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = event_ ? client_->SetEventHandle(event_) : E_OUTOFMEMORY;
  }
  if (FAILED(hr)) {
    if (error) *error = L"cannot record from the device: " + hr_text(hr);
    const std::wstring keep = endpoint_;
    close();
    endpoint_ = keep;
    return hr;
  }
  return finish_open(error);
}

HRESULT Capture::open_process(DWORD pid, bool include_tree, unsigned prefer_rate, std::wstring* error) {
  close();
  if (!process_loopback_supported()) {
    if (error) *error = kNeedsWin2004;
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  }
  // A process loopback client has no mix format of its own: ask for one
  // (Windows converts), float at Virgil's rate first, the documented 44.1
  // kHz 16-bit last.
  struct Try {
    unsigned rate;
    bool is_float;
    unsigned bits;
    bool convert;          // AUTOCONVERTPCM | SRC_DEFAULT_QUALITY
    REFERENCE_TIME buffer;  // 100 ns units
  };
  const unsigned r0 = prefer_rate >= 8000 && prefer_rate <= 384000 ? prefer_rate : 48000;
  const Try tries[] = {
      {r0, true, 32, true, 1000000},
      {48000, true, 32, true, 1000000},
      {44100, false, 16, false, 200000},  // exactly what Microsoft's sample uses
  };
  HRESULT hr = E_FAIL;
  for (const Try& t : tries) {
    client_.reset();
    hr = activate_process_loopback(pid, include_tree, client_.put());
    if (FAILED(hr)) {
      if (error) *error = L"cannot attach to the app's audio: " + hr_text(hr);
      close();
      return hr;
    }
    WAVEFORMATEXTENSIBLE wf = make_format(t.rate, 2, t.is_float, t.bits);
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        (t.convert ? AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY : 0);
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, t.buffer, 0, &wf.Format, nullptr);
    if (SUCCEEDED(hr)) {
      describe_format(&wf.Format, &fmt_);
      event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      hr = event_ ? client_->SetEventHandle(event_) : E_OUTOFMEMORY;
      if (SUCCEEDED(hr)) return finish_open(error);
      if (event_) CloseHandle(event_);
      event_ = nullptr;
    }
  }
  if (error) *error = L"cannot capture the app's audio: " + hr_text(hr);
  close();
  return hr;
}

HRESULT Capture::start() { return client_ ? client_->Start() : E_POINTER; }

bool Capture::next_packet(unsigned out_ch, Packet* p, HRESULT* out_hr) {
  if (!cap_) {
    *out_hr = E_POINTER;
    return false;
  }
  UINT32 next = 0;
  HRESULT hr = cap_->GetNextPacketSize(&next);
  if (FAILED(hr) || next == 0) {
    *out_hr = FAILED(hr) ? hr : S_OK;
    return false;
  }
  BYTE* data = nullptr;
  UINT32 n = 0;
  DWORD flags = 0;
  UINT64 devpos = 0, qpc = 0;
  hr = cap_->GetBuffer(&data, &n, &flags, &devpos, &qpc);
  if (hr == AUDCLNT_S_BUFFER_EMPTY || (SUCCEEDED(hr) && n == 0)) {
    if (SUCCEEDED(hr) && hr != AUDCLNT_S_BUFFER_EMPTY) cap_->ReleaseBuffer(0);
    *out_hr = S_OK;
    return false;
  }
  if (FAILED(hr)) {
    *out_hr = hr;
    return false;
  }
  const size_t need = size_t(n) * out_ch;
  if (scratch_.size() < need) scratch_.resize(need);
  if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) std::fill(scratch_.begin(), scratch_.begin() + long(need), 0.f);
  else if (sel_count_) {
    // The chosen channels as they are (out_ch == sel_count_).
    const unsigned bps = fmt_.bits / 8;
    for (unsigned k = 0; k < out_ch; ++k) {
      const unsigned c = sel_first_ + k;
      if (k < sel_count_ && c < fmt_.channels)
        asioconv::to_float(fmt_.sample_type, data + size_t(c) * bps, fmt_.block, n, scratch_.data() + k, out_ch);
      else
        for (UINT32 i = 0; i < n; ++i) scratch_[size_t(i) * out_ch + k] = 0.f;
    }
  } else read_frames(data, fmt_, n, scratch_.data(), out_ch);
  hr = cap_->ReleaseBuffer(n);
  const int64_t now = virgil::mono_ns();
  // u64QPCPosition: the first frame's performance-counter time in 100 ns
  // units (the clock behind mono_ns()). Used when plausible.
  const int64_t first_ns = int64_t(qpc) * 100;
  const int64_t len_ns = int64_t(double(n) * 1e9 / fmt_.rate);
  const bool stamped = qpc != 0 && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && first_ns + len_ns > now - 1000000000LL &&
                       first_ns < now + 100000000LL;
  p->data = scratch_.data();
  p->frames = n;
  p->end_ns = stamped ? first_ns + len_ns : now;
  p->flags = flags;
  p->stamped = stamped;
  *out_hr = hr;
  return SUCCEEDED(hr);
}

// ---- render ------------------------------------------------------------------------

void Render::close() {
  if (client_) client_->Stop();
  render_.reset();
  client_.reset();
  if (event_) CloseHandle(event_);
  event_ = nullptr;
  frames_ = 0;
  endpoint_.clear();
}

HRESULT Render::open(const std::wstring& endpoint, REFERENCE_TIME buffer_hns, std::wstring* error) {
  close();
  Com<IMMDevice> dev;
  HRESULT hr = open_render_endpoint(endpoint, dev.put());
  if (FAILED(hr)) {
    if (error) *error = L"output device not available: " + hr_text(hr);
    return hr;
  }
  endpoint_ = endpoint_id(dev.get());
  WAVEFORMATEX* mix = nullptr;
  hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.put_void());
  if (SUCCEEDED(hr)) hr = client_->GetMixFormat(&mix);
  if (SUCCEEDED(hr) && !describe_format(mix, &fmt_)) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
  if (SUCCEEDED(hr))
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                             buffer_hns, 0, mix, nullptr);
  if (mix) CoTaskMemFree(mix);
  if (SUCCEEDED(hr)) {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = event_ ? client_->SetEventHandle(event_) : E_OUTOFMEMORY;
  }
  if (SUCCEEDED(hr)) hr = client_->GetBufferSize(&frames_);
  if (SUCCEEDED(hr)) hr = client_->GetService(__uuidof(IAudioRenderClient), render_.put_void());
  if (FAILED(hr)) {
    if (error) *error = L"cannot play on the output device: " + hr_text(hr);
    close();
    return hr;
  }
  return S_OK;
}

HRESULT Render::start() { return client_ ? client_->Start() : E_POINTER; }

HRESULT Render::writable(UINT32* n) {
  *n = 0;
  if (!client_) return E_POINTER;
  UINT32 pad = 0;
  const HRESULT hr = client_->GetCurrentPadding(&pad);
  if (SUCCEEDED(hr)) *n = pad < frames_ ? frames_ - pad : 0;
  return hr;
}

HRESULT Render::write(const float* src, unsigned src_ch, UINT32 n) {
  if (!render_) return E_POINTER;
  if (n == 0) return S_OK;
  BYTE* data = nullptr;
  HRESULT hr = render_->GetBuffer(n, &data);
  if (FAILED(hr)) return hr;
  if (src) write_frames(src, src_ch, n, fmt_, data);
  return render_->ReleaseBuffer(n, src ? 0 : DWORD(AUDCLNT_BUFFERFLAGS_SILENT));
}

HRESULT Render::write_channels(const float* src, unsigned src_ch, unsigned first0, UINT32 n) {
  if (!render_) return E_POINTER;
  if (n == 0) return S_OK;
  BYTE* data = nullptr;
  HRESULT hr = render_->GetBuffer(n, &data);
  if (FAILED(hr)) return hr;
  if (src) {
    std::memset(data, 0, size_t(n) * fmt_.block);
    const unsigned bps = fmt_.bits / 8;
    for (unsigned k = 0; k < src_ch && first0 + k < fmt_.channels; ++k)
      asioconv::from_float(fmt_.sample_type, src + k, src_ch, n, data + size_t(first0 + k) * bps, fmt_.block);
  }
  return render_->ReleaseBuffer(n, src ? 0 : DWORD(AUDCLNT_BUFFERFLAGS_SILENT));
}

// ---- device changes ------------------------------------------------------------------

class DeviceWatcher::Client final : public IMMNotificationClient {
 public:
  explicit Client(DeviceWatcher* w) : w_(w) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IMMNotificationClient))) {
      *ppv = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return bump(false); }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return bump(false); }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return bump(false); }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override {
    if (DeviceWatcher* w = w_.load(std::memory_order_acquire))
      if (flow == eCapture || flow == eAll) w->capture_default_changes_.fetch_add(1, std::memory_order_acq_rel);
    return bump(flow == eRender || flow == eAll);
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
  void detach() { w_.store(nullptr, std::memory_order_release); }

 private:
  HRESULT bump(bool dflt) {
    if (DeviceWatcher* w = w_.load(std::memory_order_acquire)) {
      w->changes_.fetch_add(1, std::memory_order_acq_rel);
      if (dflt) w->default_changes_.fetch_add(1, std::memory_order_acq_rel);
    }
    return S_OK;
  }
  std::atomic<ULONG> refs_{1};
  std::atomic<DeviceWatcher*> w_;
};

bool DeviceWatcher::start() {
  stop();
  if (FAILED(create_enumerator(enumerator_.put()))) return false;
  client_ = new Client(this);
  if (FAILED(enumerator_->RegisterEndpointNotificationCallback(client_))) {
    client_->Release();
    client_ = nullptr;
    enumerator_.reset();
    return false;
  }
  return true;
}

void DeviceWatcher::stop() {
  if (client_) {
    if (enumerator_) enumerator_->UnregisterEndpointNotificationCallback(client_);
    client_->detach();  // a callback still in flight no longer reaches us
    client_->Release();
    client_ = nullptr;
  }
  enumerator_.reset();
}

}  // namespace wa
