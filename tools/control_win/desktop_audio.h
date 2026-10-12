// Desktop audio for Virgil Control (Windows): Windows apps -> Virgil's Dante
// transmit channels, and Dante receive channels -> a Windows output device.
//
// Everything is user-space WASAPI (no driver): loopback capture of an output
// device, or of one app's process tree (process loopback, Windows 10 2004+).
// Each source goes through an AdaptiveBridge (src/common/resampler.cpp) that
// converts the device's rate to Virgil's and follows the drift between the
// device clock and the Dante media clock. One Virgil playback slot ("Desktop
// audio") carries the mix, written ahead of the transmit horizon on Virgil's
// media clock exactly like the ASIO driver does.
//
// Threads (all owned by Engine, none blocks the UI thread):
//   supervisor   connects to Virgil, (re)builds a Session when the settings
//                change or virgild restarts / stops / changes its layout
//   I/O          media-clock thread (MMCSS Pro Audio): pulls every source's
//                bridge, mixes, writes Virgil's ring; reads receive channels
//   capture      one per source (MMCSS): WASAPI loopback -> its bridge(s)
//   output       (optional, MMCSS): bridge -> WASAPI shared-mode render
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "desktop_audio_settings.h"

namespace da {

struct SourceStatus {
  std::wstring text;    // what the source is doing, or what is wrong
  bool error = false;
  bool sound = false;   // audio above silence in the last half second
  float peak = 0;       // linear peak since the previous status()
};

struct Status {
  enum Phase { kOff, kWaiting, kRunning, kError };
  Phase phase = kOff;
  std::wstring text;  // one status line
  bool connected = false;
  unsigned rate = 0, tx = 0, rx = 0;  // Virgil's layout, when connected
  std::vector<std::string> tx_names, rx_names;
  std::vector<SourceStatus> sources;  // same order as the settings
  SourceStatus output;
  bool app_capture_supported = true;  // Windows 10 2004+
};

class Engine {
 public:
  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void start(const vc::DesktopAudioSettings& s);  // once, at startup
  void apply(const vc::DesktopAudioSettings& s);  // never blocks
  void shutdown();                                // joins every thread (at exit)
  Status status();                                // cheap; drains the peaks
  vc::DesktopAudioSettings settings();            // the last applied

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// %APPDATA%\Virgil\desktop-audio.conf
std::wstring settings_path();
// Defaults if the file is missing or unreadable (a reason in *error then).
vc::DesktopAudioSettings load_settings(std::wstring* error);
bool save_settings(const vc::DesktopAudioSettings& s, std::wstring* error);

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& w);

}  // namespace da
