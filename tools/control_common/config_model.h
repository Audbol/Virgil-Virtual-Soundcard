// Virgil Control (all platforms): the channel name editor and the plain
// configuration file editor ("Edit configuration…"), on top of virgild's
// GET / POST /api/config.
//
// Channel names live in the configuration file, [channel_names] tx1 = Kick,
// rx12 = Talkback (index 1 = channel 1; no line = the default "TX n" /
// "RX n"). Renames made in Dante Controller are written there by virgild
// too, so editors load the configuration afresh each time they open.
//
// Saving POSTs the whole file; virgild validates it (400 with a message on
// failure), writes it and restarts the audio engine itself. A save carries
// the version of the text it is based on (X-Virgil-Config-Version), so a
// file changed meanwhile (Stop, a rename in Dante Controller, another
// editor) is refused with 409 instead of being overwritten.
#pragma once

#include <string>
#include <vector>

namespace vc {

// --- configuration text --------------------------------------------------------
struct ConfigText {
  std::string text;     // the file as virgild has it (or its running defaults)
  std::string version;  // X-Virgil-Config-Version of `text` ("" from old daemons)
};
// GET /api/config.
bool fetch_config(unsigned port, ConfigText* out, std::string* error);

struct SaveResult {
  bool ok = false;
  bool conflict = false;  // 409: changed elsewhere since `base_version`; reload
  std::string error;      // the daemon's message ("line 3: unknown key 'x'", ...)
  std::string path;       // where it was saved
  bool enabled = true;    // false: Virgil is stopped, settings apply when started
};
// POST /api/config with `text`. `base_version` ("" = save regardless) is the
// ConfigText::version the edit started from.
SaveResult save_config_text(unsigned port, const std::string& text, const std::string& base_version);
// A line for the status label after a successful save, e.g. "Saved to
// /etc/virgil/virgil.conf. Audio engine restarting…".
std::string saved_message(const SaveResult& r);

// --- channel names -------------------------------------------------------------
constexpr unsigned kMaxNameLength = 31;

struct ChannelNames {
  unsigned tx = 8, rx = 8;  // channel counts ([device] tx_channels / rx_channels)
  // One entry per channel: the configured name, "" = default. Edit in place.
  std::vector<std::string> tx_names, rx_names;
  std::vector<std::string>& names(bool tx_dir) { return tx_dir ? tx_names : rx_names; }
  const std::vector<std::string>& names(bool tx_dir) const { return tx_dir ? tx_names : rx_names; }
};

// "TX 1" / "RX 1" (1-based), as an unnamed channel shows in Dante Controller.
std::string default_channel_name(bool tx, unsigned number);
// Why `name` cannot be a channel name, or "" if it can (the daemon's rules:
// 1 to 31 printable ASCII characters, no leading / trailing space, none of
// = . @ " \). Same messages as virgild.
std::string channel_name_problem(const std::string& name);
// Leading and trailing spaces removed (as the editors take a field's text).
std::string trim_name(const std::string& s);

// The names in a configuration text (/api/config).
ChannelNames load_channel_names(const std::string& conf);

struct NameError {
  bool tx = true;
  unsigned index = 0;   // 0-based channel index
  std::string message;  // for the field: "names cannot contain '@'", "also the name of channel 3"
};
// Per-field problems of the (trimmed) names, empty when all can be saved.
// Matches virgild's validation: valid names, unique per direction ignoring
// case, also against the default names of unnamed channels.
std::vector<NameError> validate_channel_names(const ChannelNames& n);
// The error for one field, or "".
std::string name_error(const std::vector<NameError>& errors, bool tx, unsigned index);

// `conf` with the names written into [channel_names] (trimmed; "" removes
// the line). With `base` (what the editor loaded), only the names changed
// since then are written, so renames made meanwhile in Dante Controller for
// other channels are kept.
std::string apply_channel_names(const std::string& conf, const ChannelNames& names,
                                const ChannelNames* base = nullptr);

// Validates, then loads the current configuration, applies the changed names
// (relative to `base`) and saves it; retries a few times when the file
// changed meanwhile. On failure `error` holds a message for the user.
bool save_channel_names(unsigned port, const ChannelNames& base, const ChannelNames& edited, std::string* error);

// Placeholder for a field: the channel's name on the network (from
// Status::tx_names / rx_names) or the default.
std::string channel_name_placeholder(const std::vector<std::string>& live_names, bool tx, unsigned index);

}  // namespace vc
