// Desktop audio settings (Virgil Control): which of the computer's audio
// goes to which Virgil transmit channels, and which receive channels play on
// an output device. Stored per user in an INI file in the style of
// virgil.conf (Windows %APPDATA%\Virgil\desktop-audio.conf, Linux
// ~/.config/virgil/desktop-audio.conf, macOS ~/Library/Application
// Support/Virgil/desktop-audio.conf). The same format everywhere; only what
// identifies a device or an app differs (Windows: endpoint ids and exe
// names; Linux: PulseAudio / PipeWire sink names and app names; macOS: Core
// Audio device UIDs and app bundle IDs or names):
//
//   [desktop-audio]
//   enabled = true
//
//   [source]                 # one section per source
//   type = device            # device: everything an output device plays
//   device = default         #   default (follows the system) or a device id
//   name = "Speakers"        #   the device's name when it was chosen
//   channel = 1              # first Virgil transmit channel (1-based)
//   mono = false             # true: one channel, left and right mixed
//   gain_db = 0
//   mute = false
//
//   [source]
//   type = app               # app: one program (Windows: and its child processes)
//   app = chrome.exe         #   Windows: executable name; Linux: app name ("Firefox") or program
//   channel = 3
//
//   [source]
//   type = all-apps          # every program except Virgil Control itself
//
//   [source]
//   type = input             # input: an audio interface's inputs, one channel each
//   api = asio               #   Windows: asio (the interface's ASIO driver) or wasapi;
//                            #   ignored elsewhere (Core Audio, PulseAudio / PipeWire)
//   device = "Focusrite USB ASIO"  # ASIO: driver name; WASAPI: endpoint id ("default" =
//                            #   default recording device); macOS: device UID; Linux: source name
//   name = "Scarlett 18i20"  #   the device's name when it was chosen
//   first_input = 1          #   first input of the interface (1-based)
//   inputs = 8               #   how many inputs: first_input.. -> channel.. (1-64)
//   buffer = 0               #   ASIO buffer in frames (0 = the driver's preferred size)
//   channel = 1              #   first Virgil transmit channel
//
//   [output]                 # Dante receive channels -> an output device
//   enabled = false
//   device = default
//   channel = 1              # first receive channel (a pair unless mono)
//   mono = false
//   gain_db = 0
//
// Portable (no Windows APIs) so the unit tests cover the parser.
#pragma once

#include <string>
#include <vector>

namespace vc {

constexpr size_t kMaxDesktopSources = 16;
constexpr unsigned kMaxInterfaceInputs = 64;
constexpr double kMinGainDb = -60, kMaxGainDb = 12;

struct DesktopAudioSource {
  enum Type { kDevice, kApp, kAllApps, kInput };
  Type type = kDevice;
  std::string device = "default";  // kDevice: "default" or a device id (UTF-8)
  std::string name;                // kDevice: friendly name, for display while unplugged
  std::string app;                 // kApp: chrome.exe (Windows), Firefox (Linux), com.spotify.client (macOS)
  unsigned channel = 1;            // first Virgil transmit channel, 1-based
  bool mono = false;               // kDevice / kApp / kAllApps: one channel, left + right
  double gain_db = 0;
  bool mute = false;
  // kInput: an audio interface's inputs, first_input.. -> channel.., one each.
  std::string api;                 // Windows: "asio" (default) or "wasapi"; "" elsewhere
  unsigned first_input = 1;        // 1-based
  unsigned inputs = 2;             // 1..kMaxInterfaceInputs
  unsigned buffer = 0;             // ASIO buffer in frames, 0 = the driver's preferred

  unsigned width() const { return type == kInput ? inputs : mono ? 1u : 2u; }
  // Same capture (type, device or app, channels); gain and mute may differ.
  bool same_route(const DesktopAudioSource& o) const {
    return type == o.type && device == o.device && app == o.app && channel == o.channel && mono == o.mono &&
           api == o.api && first_input == o.first_input && inputs == o.inputs && buffer == o.buffer;
  }
  bool operator==(const DesktopAudioSource& o) const {
    return same_route(o) && name == o.name && gain_db == o.gain_db && mute == o.mute;
  }
};

struct DesktopAudioOutput {
  bool enabled = false;
  std::string device = "default";
  std::string name;
  unsigned channel = 1;  // first Virgil receive channel, 1-based
  bool mono = false;     // one receive channel on both sides
  double gain_db = 0;

  bool same_route(const DesktopAudioOutput& o) const {
    return enabled == o.enabled && device == o.device && channel == o.channel && mono == o.mono;
  }
  bool operator==(const DesktopAudioOutput& o) const {
    return same_route(o) && name == o.name && gain_db == o.gain_db;
  }
};

struct DesktopAudioSettings {
  bool enabled = false;  // master switch
  std::vector<DesktopAudioSource> sources;
  DesktopAudioOutput output;

  bool operator==(const DesktopAudioSettings& o) const {
    return enabled == o.enabled && sources == o.sources && output == o.output;
  }
  bool operator!=(const DesktopAudioSettings& o) const { return !(*this == o); }
  // Only gains / mutes / names differ: can be applied without restarting.
  bool same_routes(const DesktopAudioSettings& o) const;
};

// Never fails on bad input: unknown sections and keys, bad values and
// sources without what they need are skipped (or clamped), each with a line
// in `warnings`. Returns false only if out is null.
bool parse_desktop_audio(const std::string& text, DesktopAudioSettings* out, std::vector<std::string>* warnings);
std::string format_desktop_audio(const DesktopAudioSettings& s);

const char* source_type_key(DesktopAudioSource::Type t);  // "device", "app", "all-apps"
double db_to_gain(double db);  // linear; 0 for <= kMinGainDb

}  // namespace vc
