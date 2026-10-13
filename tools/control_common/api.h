// Virgil Control (all platforms): talking to virgild's local control API.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace vc {

// --- minimal JSON ------------------------------------------------------------
struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;

  const Json& operator[](const char* key) const;
  const Json& operator[](size_t i) const;
  double num(double def = 0) const { return type == Number ? n : def; }
  std::string str(const std::string& def = "") const { return type == String ? s : def; }
  bool boolean(bool def = false) const { return type == Bool ? b : def; }
  size_t size() const { return type == Array ? a.size() : 0; }
};
bool parse_json(const std::string& text, Json* out);

// --- HTTP to 127.0.0.1:port ---------------------------------------------------
struct HttpResult {
  int status = 0;  // 0 = no connection
  std::string body;
  std::string headers;  // response header lines ("Name: value\r\n" each)
  // Value of response header `name` (case-insensitive), or "".
  std::string header(const char* name) const;
};
// `extra_headers`: more request header lines, each ending in "\r\n".
HttpResult http(unsigned port, const char* method, const std::string& path,
                const std::string& body = std::string(), const std::string& extra_headers = std::string());
// The daemon's {"error": "..."} message of a failed request, or a message for
// the user when the service did not answer.
std::string http_error(const HttpResult& r);

// --- virgil.conf editing that keeps comments and layout -----------------------
// As the daemon reads it: [device] and [network] are one section, keys known
// under a second name (playback_channels, capture_channels, rx_latency_us)
// match too, and the last occurrence wins. conf_set changes every occurrence
// (or adds the key to the section).
std::string conf_get(const std::string& text, const std::string& section, const std::string& key);
std::string conf_set(const std::string& text, const std::string& section, const std::string& key,
                     const std::string& value);
// Removes every occurrence of `key` from `section` (as conf_get finds it).
std::string conf_remove(const std::string& text, const std::string& section, const std::string& key);

// --- daemon status, as shown by the UI ----------------------------------------
struct Client {
  std::string name;
  bool active = false;
};
struct Status {
  bool reachable = false;
  std::string version, state, error, config_path;
  std::string name, address, iface;
  unsigned rate = 48000, tx = 0, rx = 0, latency_us = 0, tx_latency_us = 0;
  bool dante = false;
  std::string clock;  // stopped | free-run | ptp-locked | ptp-master
  std::string clock_source;  // ptp | free (settings: local clock only)
  double offset_us = 0;
  std::string grandmaster;
  // What the PTP follower has heard ("clock.ptp"; absent from older daemons,
  // with clock = free, while stopped or while Virgil leads the clock).
  bool ptp_info = false;
  unsigned ptp_senders = 0;       // hosts heard sending PTP
  bool ptp_master_heard = false;  // a clock leader was followed since start
  double no_leader_s = 0;         // seconds without a clock leader
  std::string ptp_adapter;        // OS name of the adapter Virgil runs on
  unsigned long long late_ticks = 0, clock_steps = 0;
  unsigned long long dropouts_last_hour = 0;
  std::vector<std::string> tx_names, rx_names;  // channel names (Dante Controller)
  std::vector<Client> clients;
  std::vector<float> meter_tx, meter_rx;
};
bool parse_status(const std::string& body, Status* st);

// "No Dante clock leader" notice: running with clock = ptp, neither locked
// nor leading, and no leader heard for more than 15 s. The same words in
// every app (the web panel has a copy in index.html).
struct ClockNotice {
  bool show = false;
  std::string headline;  // short, for the header's clock line ("No Dante clock leader")
  std::string title;     // "No Dante clock leader found on eth0 (192.168.1.5)"
  std::vector<std::string> hints;  // likely fixes, one short sentence each
};
constexpr double kNoLeaderNoticeS = 15;
ClockNotice clock_notice(const Status& s);
// `in` with invalid UTF-8 sequences replaced by U+FFFD.
std::string valid_utf8(const std::string& in);

struct Interface {
  std::string name, address;
  bool loopback = false, virt = false;
};
std::vector<Interface> parse_interfaces(const std::string& body);

}  // namespace vc
