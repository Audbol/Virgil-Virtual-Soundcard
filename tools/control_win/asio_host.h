// ASIO host for Virgil Control's desktop audio (Windows): loads an audio
// interface's own ASIO driver (the only ASIO driver Virgil Control loads;
// Virgil itself is reached through shared memory, not ASIO) and moves the
// interface's inputs and outputs to and from the engine with the driver's
// buffer, so an audio interface reaches Dante with a few ms of latency.
//
// Threads and COM: every call into the driver happens on the host's own
// thread, which enters a single-threaded apartment (what ASIO drivers expect:
// many are apartment-threaded COM objects that create windows or timers),
// owns the hidden window passed to init() and pumps its messages while the
// driver runs. The driver calls bufferSwitch on its own audio thread; that is
// where Callback::process runs (no locks, no allocation).
//
// ASIO's callbacks carry no user pointer, so one host can be active per
// process ("only one ASIO driver at a time"); one host serves both an input
// source and the interface output when they name the same driver.
//
// Without the Steinberg ASIO SDK (VIRGIL_HAVE_ASIO_SDK undefined) the driver
// list still works (registry), but open() and probe() fail with
// kNotBuiltIn.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace ah {

extern const wchar_t* const kNotBuiltIn;  // "ASIO support was not built in ..."
bool built_in();

// Installed ASIO drivers (HKLM\SOFTWARE\ASIO, this process's registry view:
// a 64-bit Virgil Control sees and can load 64-bit drivers only). Virgil's
// own ASIO driver is left out.
struct Driver {
  std::wstring name;   // the registry key name: what the settings store
  std::wstring clsid;  // "{...}"
  std::wstring description;
};
struct DriverList {
  std::vector<Driver> drivers;
  // Drivers registered only for the other bitness (32-bit ones, from a
  // 64-bit Virgil Control): cannot be loaded here.
  std::vector<std::wstring> other_bitness;
};
DriverList list_drivers();

// What a driver offers (loaded briefly on a thread of its own; blocks for as
// long as the driver takes, so call it off the UI thread). If the engine is
// hosting that driver right now, its current details instead.
struct Probe {
  bool ok = false;
  std::wstring error;
  unsigned inputs = 0, outputs = 0;
  std::vector<std::wstring> input_names, output_names;
  double rate = 0;
  long min_buffer = 0, max_buffer = 0, pref_buffer = 0, granularity = 0;
};
Probe probe(const std::wstring& driver);

// The engine's side of a running host. prepare() is called on the host
// thread once the format is known, before the driver starts (set up the
// bridges there); process() on the driver's audio thread for every buffer.
class Callback {
 public:
  virtual ~Callback() = default;
  // rate: the interface's; frames: the ASIO buffer; inputs / outputs: the
  // channels actually opened. False: do not start.
  virtual bool prepare(double rate, unsigned frames, unsigned inputs, unsigned outputs) = 0;
  // in: `inputs` interleaved channels (null if none); out: fill `outputs`
  // interleaved channels (null if none). end_ns: mono_ns() time of the last
  // input frame.
  virtual void process(const float* in, float* out, size_t frames, int64_t end_ns) = 0;
};

struct Config {
  std::wstring driver;                // registry name
  unsigned first_input = 1, inputs = 0;    // 1-based; 0 inputs = none
  unsigned first_output = 1, outputs = 0;  // 1-based; 0 outputs = none
  unsigned buffer = 0;                // frames, 0 = the driver's preferred
  double rate = 48000;                // wanted (Virgil's); else the driver's own
};

class Host {
 public:
  Host() = default;
  ~Host() { close(); }
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  // Loads, sets up and starts the driver; blocks until it runs or failed
  // (bounded wait). The callback must outlive close().
  bool open(const Config& cfg, Callback* cb, std::wstring* error);
  void close();  // stops and unloads the driver (joins the host thread)
  bool is_open() const { return th_.joinable(); }

  // After a successful open().
  double rate() const { return rate_; }
  unsigned buffer() const { return frames_; }
  unsigned inputs() const { return nin_; }
  unsigned outputs() const { return nout_; }
  unsigned device_inputs() const { return dev_in_; }
  unsigned device_outputs() const { return dev_out_; }
  bool inputs_clamped() const { return in_clamped_; }
  bool outputs_clamped() const { return out_clamped_; }
  const std::wstring& note() const { return note_; }  // e.g. a buffer size the driver refused
  // The driver asked to be reset (kAsioResetRequest, a sample rate change):
  // close() and open() again.
  bool reset_requested() const { return reset_.load(std::memory_order_acquire); }
  uint64_t buffers() const { return switches_.load(std::memory_order_relaxed); }  // bufferSwitch count
  uint64_t resyncs() const { return resyncs_.load(std::memory_order_relaxed); }

  // ---- the driver's callbacks (asio_host.cpp; public for the C trampolines)
  void on_buffer(long index, const void* time_info);
  void on_reset() { reset_.store(true, std::memory_order_release); }
  void on_resync() { resyncs_.fetch_add(1, std::memory_order_relaxed); }

 private:
  void thread_main();

  Config cfg_;
  Callback* cb_ = nullptr;
  std::thread th_;
  std::atomic<DWORD> tid_{0};
  HANDLE ready_ = nullptr;
  bool ok_ = false;
  std::wstring error_;
  // Set up on the host thread before start(), read by the audio thread.
  void* drv_ = nullptr;  // IASIO*
  std::vector<void*> bufs_[2];  // per opened channel (inputs first), per half
  std::vector<long> types_;
  std::vector<unsigned> bytes_;
  std::vector<float> in_, out_;
  double rate_ = 0;
  unsigned frames_ = 0, nin_ = 0, nout_ = 0, dev_in_ = 0, dev_out_ = 0;
  bool in_clamped_ = false, out_clamped_ = false, output_ready_ = false;
  std::wstring note_;
  std::atomic<bool> armed_{false}, reset_{false};
  std::atomic<uint64_t> switches_{0}, resyncs_{0};
};

}  // namespace ah
