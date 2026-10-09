#include "settings_model.h"

#include <cstdio>
#include <cstdlib>

namespace vc {

const char* const kRates[4] = {"44100", "48000", "88200", "96000"};
const char* const kRateLabels[4] = {"44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz"};
const char* const kRxLatencies[6] = {"1", "2", "4", "5", "10", "20"};
const char* const kTxLatencies[6] = {"3.5", "4", "5", "6", "10", "20"};

namespace {

std::string ms_text(const std::string& us) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", std::atof(us.c_str()) / 1000.0);
  return b;
}

std::string us_text(const std::string& ms) {
  const double v = std::strtod(ms.c_str(), nullptr);
  return std::to_string(long(v * 1000 + 0.5));
}

bool truthy(const std::string& v) { return v == "true" || v == "1" || v == "yes" || v == "on"; }

unsigned channels(const std::string& v) {
  const long n = std::strtol(v.c_str(), nullptr, 10);
  return n > 0 ? unsigned(n) : 8;
}

}  // namespace

std::vector<std::string> SettingsForm::iface_labels() const {
  std::vector<std::string> out{"Automatic (prefers wired Ethernet)"};
  for (const auto& f : ifaces)
    out.push_back(f.name + "  (" + f.address + ")" + (f.virt ? "  virtual" : ""));
  return out;
}

SettingsForm load_settings(const std::string& c, const std::vector<Interface>& all_ifaces) {
  SettingsForm f;
  const std::string name = conf_get(c, "device", "name");
  if (!name.empty()) f.name = name;
  const std::string cur_if = conf_get(c, "device", "interface");
  for (const auto& i : all_ifaces) {
    if (i.loopback) continue;
    f.ifaces.push_back(i);
    if (!cur_if.empty() && (cur_if == i.name || cur_if == i.address)) f.iface_index = int(f.ifaces.size());
  }
  if (!cur_if.empty() && f.iface_index == 0) {
    // Configured but not present right now: keep it selectable.
    Interface missing;
    missing.name = cur_if;
    missing.address = "not connected";
    f.ifaces.push_back(missing);
    f.iface_index = int(f.ifaces.size());
  }
  const std::string rate = conf_get(c, "device", "sample_rate");
  for (int k = 0; k < 4; ++k)
    if (rate == kRates[k]) f.rate_index = k;
  f.tx_channels = channels(conf_get(c, "device", "tx_channels"));
  f.rx_channels = channels(conf_get(c, "device", "rx_channels"));
  const std::string rl = conf_get(c, "device", "latency_us"), tl = conf_get(c, "device", "tx_latency_us");
  if (!rl.empty()) f.rx_latency_ms = ms_text(rl);
  if (!tl.empty()) f.tx_latency_ms = ms_text(tl);
  f.local_clock = conf_get(c, "device", "clock") == "free";
  f.master_capable = truthy(conf_get(c, "ptp", "master_capable"));
  return f;
}

std::string apply_settings(const std::string& conf, const SettingsForm& f) {
  std::string c = conf;
  c = conf_set(c, "device", "name", f.name.substr(0, 31));
  std::string iface;
  if (f.iface_index > 0 && size_t(f.iface_index) <= f.ifaces.size()) iface = f.ifaces[size_t(f.iface_index) - 1].name;
  c = conf_set(c, "device", "interface", iface);
  c = conf_set(c, "device", "sample_rate", kRates[f.rate_index < 0 || f.rate_index > 3 ? 1 : f.rate_index]);
  c = conf_set(c, "device", "tx_channels", std::to_string(f.tx_channels));
  c = conf_set(c, "device", "rx_channels", std::to_string(f.rx_channels));
  c = conf_set(c, "device", "latency_us", us_text(f.rx_latency_ms));
  c = conf_set(c, "device", "tx_latency_us", us_text(f.tx_latency_ms));
  c = conf_set(c, "device", "clock", f.local_clock ? "free" : "ptp");
  c = conf_set(c, "ptp", "master_capable", f.master_capable ? "true" : "false");
  return c;
}

bool save_settings(unsigned port, const std::string& conf, std::string* error) {
  const auto r = http(port, "POST", "/api/config", conf);
  if (r.status == 200) return true;
  Json j;
  std::string err = "Could not reach the Virgil service.";
  if (r.status && parse_json(r.body, &j)) err = j["error"].str("HTTP " + std::to_string(r.status));
  if (error) *error = err;
  return false;
}

}  // namespace vc
