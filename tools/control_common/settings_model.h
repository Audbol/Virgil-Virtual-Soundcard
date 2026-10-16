// Virgil Control (all platforms): the settings dialog's fields, read from and
// written back to virgil.conf text (comments and layout are kept).
#pragma once

#include <string>
#include <vector>

#include "api.h"
#include "diagnostics_model.h"

namespace vc {

struct SettingsForm {
  std::string name = "Virgil";
  // Network interface: 0 = automatic, otherwise index + 1 into `ifaces`
  // (loopback interfaces are left out of the list).
  std::vector<Interface> ifaces;
  int iface_index = 0;
  int rate_index = 1;  // into kRates
  unsigned tx_channels = 8, rx_channels = 8;
  std::string rx_latency_ms = "4", tx_latency_ms = "4";
  // Mixing tick ([device] tick_us): the menu offers kTicks; another value
  // from a hand-edited file is kept (and listed) until changed.
  unsigned tick_us = 1000;
  bool local_clock = false;  // clock = free
  bool master_capable = false;
  bool wifi = false;  // [ptp] wifi: the clock filter for Wi-Fi networks
  // Web panel from other computers ([device] remote_access / remote_password).
  // Only this computer may change these (the daemon refuses remote clients).
  bool remote_access = false;
  std::string remote_password;

  // Labels for the interface menu, matching `iface_index`.
  std::vector<std::string> iface_labels() const;
};

extern const char* const kRates[4];
extern const char* const kRateLabels[4];
extern const char* const kRxLatencies[9];
extern const char* const kTxLatencies[7];  // at the 1 ms tick (tx_latency_presets(1000))

// Mixing tick choices: 1 ms (default) and 0.5 ms (lower latency, more CPU).
extern const unsigned kTicks[2];
extern const char* const kTickLabels[2];
// Menu entries for `f.tick_us`: kTickLabels, plus "0.25 ms (from the
// configuration file)" when the file has another value.
std::vector<std::string> tick_labels(const SettingsForm& f);
// Index of f.tick_us in tick_labels(f), and the tick of an index.
int tick_index(const SettingsForm& f);
unsigned tick_of_index(const SettingsForm& f, int index);
// The smallest transmit latency the daemon accepts at a tick, in ms: two
// ticks + 1.5 ms (3.5 ms at 1 ms, 2.5 ms at 0.5 ms; virgil::Config::
// min_tx_latency_us).
double min_tx_latency_ms(unsigned tick_us);
// Transmit latency menu entries for a tick, from its minimum up to 40 ms.
std::vector<std::string> tx_latency_presets(unsigned tick_us);
// "(3.5 ms minimum)".
std::string tx_latency_min_text(unsigned tick_us);
// Why f.tx_latency_ms cannot be saved at f.tick_us, or "".
std::string tx_latency_problem(const SettingsForm& f);
// The note for a transmit latency above 40 ms (allowed, up to 200 ms, but
// not every receiver buffers that much), or "".
std::string tx_latency_warning(const SettingsForm& f);

// The line under the transmit latency field: the daemon's suggestion (or
// its progress), "" without one. `form_tick_us` is the tick chosen in the
// dialog: a suggestion measured at another tick says so.
std::string tx_suggestion_note(const TxSuggestion& s, unsigned form_tick_us);
// Whether "Apply suggestion" should be offered (ready, different from the
// current setting, measured at the tick chosen in the dialog).
bool tx_suggestion_applicable(const TxSuggestion& s, unsigned form_tick_us);

// Fill the form from the daemon's /api/config text and /api/interfaces list.
SettingsForm load_settings(const std::string& conf, const std::vector<Interface>& all_ifaces);
// Why the remote access fields cannot be saved, or "" (the daemon's rules:
// with remote access on, a password of at least 8 characters, no quotes or
// control characters).
std::string remote_access_problem(const SettingsForm& f);
// Write the form back into the configuration text.
std::string apply_settings(const std::string& conf, const SettingsForm& f);

// POSTs the configuration; on failure `error` holds a message for the user.
bool save_settings(unsigned port, const std::string& conf, std::string* error);

}  // namespace vc
