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
};
HttpResult http(unsigned port, const char* method, const std::string& path,
                const std::string& body = std::string());

// --- virgil.conf editing that keeps comments and layout -----------------------
std::string conf_get(const std::string& text, const std::string& section, const std::string& key);
std::string conf_set(const std::string& text, const std::string& section, const std::string& key,
                     const std::string& value);

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
  double offset_us = 0;
  std::string grandmaster;
  unsigned long long late_ticks = 0, clock_steps = 0;
  std::vector<Client> clients;
  std::vector<float> meter_tx, meter_rx;
};
bool parse_status(const std::string& body, Status* st);

struct Interface {
  std::string name, address;
  bool loopback = false, virt = false;
};
std::vector<Interface> parse_interfaces(const std::string& body);

}  // namespace vc
