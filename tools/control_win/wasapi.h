// Small WASAPI layer for desktop audio (Virgil Control) and the
// desktop_audio_probe test tool: endpoint and audio-session listing, loopback
// capture of an output device or of an app's process tree (process loopback,
// Windows 10 version 2004+), shared-mode rendering, format conversion.
//
// Every function here makes COM calls: call them on a thread that called
// CoInitializeEx (the audio threads use the multithreaded apartment).
// Nothing blocks for long except activate_process_loopback (bounded wait).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace wa {

// Owning COM pointer (the bare minimum).
template <class T>
class Com {
 public:
  Com() = default;
  ~Com() { reset(); }
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  Com& operator=(Com&& o) noexcept {
    if (this != &o) {
      reset();
      p_ = o.p_;
      o.p_ = nullptr;
    }
    return *this;
  }
  void reset() {
    if (p_) p_->Release();
    p_ = nullptr;
  }
  T** put() {
    reset();
    return &p_;
  }
  void** put_void() { return reinterpret_cast<void**>(put()); }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

// CoInitializeEx for this thread's lifetime.
struct ComScope {
  HRESULT hr;
  explicit ComScope(DWORD mode = COINIT_MULTITHREADED) : hr(CoInitializeEx(nullptr, mode)) {}
  ~ComScope() {
    if (SUCCEEDED(hr)) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
};

// ---- endpoints, sessions, processes ----------------------------------------------

struct Endpoint {
  std::wstring id, name;
  bool is_default = false;
};
HRESULT create_enumerator(IMMDeviceEnumerator** out);
// Active output devices, the default one flagged.
std::vector<Endpoint> render_endpoints(HRESULT* hr = nullptr);
std::wstring default_render_id();
// "" or "default": the default output device (console role).
HRESULT open_render_endpoint(const std::wstring& id, IMMDevice** out);
std::wstring endpoint_id(IMMDevice* d);
// Recording devices (audio interface inputs), the default one flagged.
std::vector<Endpoint> capture_endpoints(HRESULT* hr = nullptr);
std::wstring default_capture_id();
// "" or "default": the default recording device (console role).
HRESULT open_capture_endpoint(const std::wstring& id, IMMDevice** out);
// The shared-mode format's channel count and rate of an endpoint (0 if it
// cannot be opened).
unsigned endpoint_channels(bool capture, const std::wstring& id, unsigned* rate = nullptr, std::wstring* error = nullptr);
std::wstring endpoint_name(IMMDevice* d);

// Programs with an audio session on an active output device (one entry per
// executable; active = playing right now). The system sounds session and
// this process are left out.
struct AudioApp {
  std::wstring exe;      // e.g. chrome.exe
  std::wstring display;  // the session's display name, if it has one
  DWORD pid = 0;
  bool active = false;
};
std::vector<AudioApp> audio_apps();

std::wstring exe_name_of(DWORD pid);  // file name only, "" if unknown
// Running processes named `exe` (case-insensitive) whose parent is not
// another `exe`: the roots of its process trees (chrome.exe's browser
// process, not its renderers).
std::vector<DWORD> root_processes(const std::wstring& exe);

DWORD windows_build();               // e.g. 19045
bool process_loopback_supported();   // Windows 10 version 2004 (build 19041) or later
extern const wchar_t* const kNeedsWin2004;

// ---- formats -----------------------------------------------------------------------

struct Format {
  unsigned rate = 0, channels = 0, bits = 0, block = 0;
  bool is_float = false;
  DWORD mask = 0;
  // Fold to stereo: per device channel, weight into left and right.
  float to_l[32] = {}, to_r[32] = {};
  // From stereo: per device channel, 0 = silence, 1 = left, 2 = right, 3 = (l + r) / 2.
  uint8_t from[32] = {};
  long sample_type = 0;  // the same layout as an ASIO sample type (asio_convert.h)
};
// False if the format is not PCM 16/24/32 bit or float 32/64 bit, or has
// more than 32 channels.
bool describe_format(const WAVEFORMATEX* wf, Format* f);
std::wstring format_text(const Format& f);  // "48 kHz, 2 ch, float32"
WAVEFORMATEXTENSIBLE make_format(unsigned rate, unsigned channels, bool is_float, unsigned bits);
// Device frames -> float, folded to out_ch (1 = mono mix, 2 = stereo).
void read_frames(const BYTE* src, const Format& f, size_t frames, float* dst, unsigned out_ch);
// Float (src_ch 1 or 2) -> device frames.
void write_frames(const float* src, unsigned src_ch, size_t frames, const Format& f, BYTE* dst);

// ---- errors ------------------------------------------------------------------------

std::wstring hr_text(HRESULT hr);  // "the audio device was removed (0x88890004)"

// ---- capture ---------------------------------------------------------------------

// An IAudioClient for the audio of process `pid` and its children
// (include_tree) or of everything except them, via ActivateAudioInterfaceAsync
// (VAD\Process_Loopback). Waits at most timeout_ms for the activation.
HRESULT activate_process_loopback(DWORD pid, bool include_tree, IAudioClient** out, DWORD timeout_ms = 5000);

// Loopback capture: what an output device plays, or what a process tree
// plays. Event driven when Windows allows it; drain() is safe to call on a
// timer as well (loopback of an idle device delivers no events).
class Capture {
 public:
  ~Capture() { close(); }
  HRESULT open_device(const std::wstring& endpoint, std::wstring* error);
  // prefer_rate: the format to ask for (Windows converts), e.g. Virgil's rate.
  // Shared-mode capture of a recording device (not loopback), all of its
  // channels; "" or "default" = the default recording device.
  HRESULT open_input(const std::wstring& endpoint, std::wstring* error);
  HRESULT open_process(DWORD pid, bool include_tree, unsigned prefer_rate, std::wstring* error);
  HRESULT start();
  void close();
  // Packets carry `count` of the device's channels from first0 (0-based) as
  // they are, instead of folded to mono / stereo: drain(count, ...).
  void select_channels(unsigned first0, unsigned count) {
    sel_first_ = first0;
    sel_count_ = count;
  }
  bool is_open() const { return static_cast<bool>(cap_); }
  HANDLE event() const { return event_; }  // null: poll
  const Format& format() const { return fmt_; }
  const std::wstring& endpoint() const { return endpoint_; }  // resolved id (device capture)

  struct Packet {
    const float* data;  // out_ch interleaved
    size_t frames;
    int64_t end_ns;     // mono_ns() at the packet's last frame
    DWORD flags;        // AUDCLNT_BUFFERFLAGS_*
    bool stamped;       // end_ns from the device's time stamp (else: when read)
  };
  // Reads every pending packet, converted to out_ch channels, and hands each
  // to sink(const Packet&). Stops at the first error (returned).
  template <class Sink>
  HRESULT drain(unsigned out_ch, Sink&& sink);

 private:
  HRESULT finish_open(std::wstring* error);
  bool next_packet(unsigned out_ch, Packet* p, HRESULT* hr);

  Com<IAudioClient> client_;
  Com<IAudioCaptureClient> cap_;
  HANDLE event_ = nullptr;
  Format fmt_;
  std::wstring endpoint_;
  std::vector<float> scratch_;
  unsigned sel_first_ = 0, sel_count_ = 0;
};

template <class Sink>
HRESULT Capture::drain(unsigned out_ch, Sink&& sink) {
  HRESULT hr = S_OK;
  Packet p;
  for (int guard = 0; guard < 256 && next_packet(out_ch, &p, &hr); ++guard) sink(p);
  return hr;
}

// ---- render ------------------------------------------------------------------------

class Render {
 public:
  ~Render() { close(); }
  HRESULT open(const std::wstring& endpoint, REFERENCE_TIME buffer_hns, std::wstring* error);
  HRESULT start();
  void close();
  HANDLE event() const { return event_; }
  const Format& format() const { return fmt_; }
  UINT32 buffer_frames() const { return frames_; }
  const std::wstring& endpoint() const { return endpoint_; }
  HRESULT writable(UINT32* n);
  HRESULT write(const float* src, unsigned src_ch, UINT32 n);  // nullptr = silence
  // src_ch interleaved channels onto the device's channels first0.. as they
  // are (an audio interface's outputs); its other channels get silence.
  HRESULT write_channels(const float* src, unsigned src_ch, unsigned first0, UINT32 n);

 private:
  Com<IAudioClient> client_;
  Com<IAudioRenderClient> render_;
  HANDLE event_ = nullptr;
  Format fmt_;
  UINT32 frames_ = 0;
  std::wstring endpoint_;
};

// ---- device changes ------------------------------------------------------------------

// Counts device changes (IMMNotificationClient); the audio threads
// compare the counters to notice them. Callbacks only touch atomics.
class DeviceWatcher {
 public:
  ~DeviceWatcher() { stop(); }
  bool start();  // on a COM-initialised thread
  void stop();
  unsigned changes() const { return changes_.load(std::memory_order_acquire); }
  unsigned default_changes() const { return default_changes_.load(std::memory_order_acquire); }
  unsigned capture_default_changes() const { return capture_default_changes_.load(std::memory_order_acquire); }

 private:
  class Client;
  Com<IMMDeviceEnumerator> enumerator_;
  Client* client_ = nullptr;
  std::atomic<unsigned> changes_{0}, default_changes_{0}, capture_default_changes_{0};
};

}  // namespace wa
