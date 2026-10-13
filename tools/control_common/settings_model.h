// Virgil Control (all platforms): the settings dialog's fields, read from and
// written back to virgil.conf text (comments and layout are kept).
#pragma once

#include <string>
#include <vector>

#include "api.h"

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
  bool local_clock = false;  // clock = free
  bool master_capable = false;
  // Web panel from other computers ([device] remote_access / remote_password).
  // Only this computer may change these (the daemon refuses remote clients).
  bool remote_access = false;
  std::string remote_password;

  // Labels for the interface menu, matching `iface_index`.
  std::vector<std::string> iface_labels() const;
};

extern const char* const kRates[4];
extern const char* const kRateLabels[4];
extern const char* const kRxLatencies[6];
extern const char* const kTxLatencies[6];

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
