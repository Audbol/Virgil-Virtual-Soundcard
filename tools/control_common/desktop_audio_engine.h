// Desktop audio for Virgil Control on Linux and macOS: what other programs
// play -> Virgil's Dante transmit channels, and Dante receive channels -> an
// ordinary output device (headphones, speakers). No driver.
//
// The same engine as the Windows app (tools/control_win/desktop_audio.cpp),
// in portable C++ (std::thread, UTF-8 std::string). Everything the operating
// system does sits behind Backend below: PulseAudio / PipeWire on Linux
// (tools/control_linux/desktop_audio_pulse.cpp), CoreAudio on macOS.
//
// Each captured stream goes through an AdaptiveBridge (src/common/resampler.cpp)
// that converts the device's rate to Virgil's and follows the drift between
// the device clock and the Dante media clock. One Virgil playback slot
// ("Desktop audio") carries the mix, written ahead of the transmit horizon on
// Virgil's media clock exactly like the drivers do.
//
// Threads (all owned by Engine, none blocks the UI thread):
//   supervisor   connects to Virgil, (re)builds a Session when the settings
//                change or virgild restarts / stops / changes its layout
//   I/O          media-clock thread (raised priority): pulls every capture
//                bridge, mixes, writes Virgil's ring; reads receive channels
//                into the output bridge
//   source       one per source: opens / reopens its capture streams, follows
//                the default device and the app's streams, status text
//   output       (optional) opens / reopens the render stream, status text
//   backend      the backend's own audio thread(s) call AudioSink::audio and
//                AudioSource::render (the bridges' producer / consumer side)
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "desktop_audio_settings.h"

namespace desk {

// ---- backend interface ------------------------------------------------------------------
//
// Contract (what a backend must do; see the Linux one for an example):
//  * Every Backend method may be called from any thread, concurrently, and
//    must return quickly (bounded waits only: connect() may wait a couple of
//    seconds for the sound server, nothing else should block). The UI calls
//    outputs() / apps() from a worker thread, the engine from its threads.
//  * open_*() returns a stream that is not running yet (no callbacks), with
//    rate() and device() known. start() makes it run; from then on the
//    backend calls the sink / source from its own audio thread, one call at a
//    time per stream (different streams may use different threads).
//  * Callbacks must not block or allocate in the engine; they get exactly the
//    channel count asked for (1 = mono mix of the device, 2 = stereo; the
//    backend folds or maps other layouts), interleaved float32, at rate().
//  * Destroying a stream stops it: when the destructor returns no callback is
//    running or will run. Never destroyed from inside its own callback.
//  * failed() turns true when the stream died (device unplugged, server
//    restarted, app quit); the engine then destroys it and opens a new one.
//  * changes() / default_changes() are counters that go up when output
//    devices or app streams come or go / when the default output changes.

struct Device {
  std::string id;    // stored in the settings ("default" = follow the system)
  std::string name;  // for people
  bool is_default = false;
};

// A program that has audio streams right now (one entry per program).
struct App {
  std::string id;    // what the settings store and app_streams() matches, e.g. "firefox"
  std::string name;  // for people, e.g. "Firefox"
  bool playing = false;  // a stream is running (not paused / corked)
};

// One capturable stream of an app (a PulseAudio sink input, a process).
struct AppStream {
  uint64_t id = 0;   // backend's identity; stable while the stream exists
  std::string app;   // App::id it belongs to
  uint32_t pid = 0;
};

struct Capabilities {
  bool app_capture = true;  // kApp sources
  bool all_apps = true;     // kAllApps sources
  std::string app_hint;     // why not, when one of them is false
};

class AudioSink {  // capture: the backend hands over what it recorded
 public:
  virtual ~AudioSink() = default;
  // data == nullptr: `frames` of silence (a gap). end_ns: virgil::mono_ns()
  // when the last frame was recorded (or the time of the call).
  virtual void audio(const float* data, size_t frames, int64_t end_ns) = 0;
};

class AudioSource {  // render: the backend asks for what to play
 public:
  virtual ~AudioSource() = default;
  // Fill `frames` interleaved frames; now_ns: when this was asked for.
  virtual void render(float* out, size_t frames, int64_t now_ns) = 0;
};

class Stream {
 public:
  virtual ~Stream() = default;  // stops it (see the contract above)
  virtual bool start(std::string* error) = 0;
  virtual bool failed(std::string* why) const = 0;
  virtual unsigned rate() const = 0;
  // The output device this stream belongs to (capture: the device whose
  // sound it records; render: the device it plays on), resolved: never
  // "default". Used to warn about feedback loops.
  virtual std::string device() const = 0;
};

class Backend {
 public:
  virtual ~Backend() = default;
  virtual std::string name() const = 0;  // "PulseAudio"
  // Loads / connects to the sound system if needed (bounded wait). False
  // with a sentence for people in *why when it is not there.
  virtual bool connect(std::string* why) = 0;
  virtual Capabilities capabilities() = 0;
  virtual std::vector<Device> outputs() = 0;  // the default one flagged
  virtual std::string default_output() = 0;   // its id, "" if none
  virtual std::vector<App> apps() = 0;        // without this process
  // The capturable streams of `app` (App::id or the name a user typed), or
  // with all = true those of every app except this process.
  virtual std::vector<AppStream> app_streams(const std::string& app, bool all) = 0;
  virtual unsigned changes() = 0;
  virtual unsigned default_changes() = 0;
  // device: an id from outputs(), or "default" / "" for the current default.
  virtual std::unique_ptr<Stream> open_device_capture(const std::string& device, unsigned channels,
                                                      AudioSink* sink, std::string* error) = 0;
  virtual std::unique_ptr<Stream> open_app_capture(const AppStream& s, unsigned channels, AudioSink* sink,
                                                   std::string* error) = 0;
  // latency_s: the device buffer to aim for.
  virtual std::unique_ptr<Stream> open_render(const std::string& device, unsigned channels, double latency_s,
                                              AudioSource* source, std::string* error) = 0;
};

// The platform's backend (Linux: PulseAudio / PipeWire, loaded at run time).
// Defined by the platform's backend file, not by the engine.
std::unique_ptr<Backend> make_system_backend();

// Raise the calling thread's scheduling for audio: Linux SCHED_FIFO when the
// limits allow it, else a lower nice value; macOS the time-constraint policy
// (period_ns is the expected wake-up period); Windows MMCSS. Best effort.
// Backends call it on their own audio threads too.
void raise_audio_thread_priority(int64_t period_ns, int rt_priority = 40);

// ---- engine ---------------------------------------------------------------------------------

struct SourceStatus {
  std::string text;   // what the source is doing, or what is wrong
  bool error = false;
  bool sound = false;  // audio above silence in the last half second
  float peak = 0;      // linear peak since the previous status()
};

struct Status {
  enum Phase { kOff, kWaiting, kRunning, kError };
  Phase phase = kOff;
  std::string text;  // one status line
  bool connected = false;
  unsigned rate = 0, tx = 0, rx = 0;  // Virgil's layout, when connected
  std::vector<std::string> tx_names, rx_names;
  std::vector<SourceStatus> sources;  // same order as the settings
  SourceStatus output;
  Capabilities caps;
};

class Engine {
 public:
  // shm_name: Virgil's shared memory ("" = the default, $VIRGIL_SHM_NAME).
  explicit Engine(std::unique_ptr<Backend> backend, std::string shm_name = std::string());
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void start(const vc::DesktopAudioSettings& s);  // once, at startup
  void apply(const vc::DesktopAudioSettings& s);  // never blocks
  void shutdown();                                // joins every thread (at exit)
  Status status();                                // cheap; drains the peaks
  vc::DesktopAudioSettings settings();            // the last applied
  Backend& backend();                             // for device / app lists (thread-safe)

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---- settings file ----------------------------------------------------------------------------

// Linux: $XDG_CONFIG_HOME/virgil/desktop-audio.conf (~/.config/virgil/...);
// macOS: ~/Library/Application Support/Virgil/desktop-audio.conf.
std::string settings_path();
// Defaults if the file is missing or unreadable (a reason in *error then,
// or the first parser warning).
vc::DesktopAudioSettings load_settings(const std::string& path, std::string* error);
// Written next to it and renamed over it (never half-written); creates the folder.
bool save_settings(const std::string& path, const vc::DesktopAudioSettings& s, std::string* error);

}  // namespace desk
