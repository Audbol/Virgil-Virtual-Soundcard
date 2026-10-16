#include "settings_model.h"

#include "config_model.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace vc {

const char* const kRates[4] = {"44100", "48000", "88200", "96000"};
const char* const kRateLabels[4] = {"44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz"};
const char* const kRxLatencies[9] = {"1", "2", "4", "5", "10", "20", "50", "100", "200"};
const char* const kTxLatencies[7] = {"3.5", "4", "5", "6", "10", "20", "40"};
const unsigned kTicks[2] = {1000, 500};
const char* const kTickLabels[2] = {"1 ms (default)", "0.5 ms (lower latency, more CPU)"};

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

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// As the daemon reads booleans.
bool truthy(const std::string& v) {
  const std::string l = lower(v);
  return l == "true" || l == "1" || l == "yes" || l == "on";
}

// 0 is a valid count for one direction (the daemon needs one channel in
// all); missing or unreadable is the default, 8.
unsigned channels(const std::string& v) {
  if (v.empty()) return 8;
  char* end = nullptr;
  const long n = std::strtol(v.c_str(), &end, 10);
  if (end == v.c_str() || *end || n < 0) return 8;
  return unsigned(n);
}

// [device] tick_us as the daemon reads it; 1000 when absent or unreadable.
unsigned conf_tick(const std::string& conf) {
  const std::string t = conf_get(conf, "device", "tick_us");
  char* end = nullptr;
  const unsigned long v = std::strtoul(t.c_str(), &end, 10);
  return !t.empty() && end != t.c_str() && !*end && v > 0 && v < 1000000 ? unsigned(v) : 1000;
}

std::string g_text(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", v);
  return b;
}

}  // namespace

std::vector<std::string> tick_labels(const SettingsForm& f) {
  std::vector<std::string> out(std::begin(kTickLabels), std::end(kTickLabels));
  if (tick_index(f) == 2) out.push_back(g_text(f.tick_us / 1000.0) + " ms (from the configuration file)");
  return out;
}

int tick_index(const SettingsForm& f) {
  for (int i = 0; i < 2; ++i)
    if (f.tick_us == kTicks[i]) return i;
  return 2;
}

unsigned tick_of_index(const SettingsForm& f, int index) {
  if (index == 0 || index == 1) return kTicks[index];
  return f.tick_us;  // the file's own value, kept
}

double min_tx_latency_ms(unsigned tick_us) { return (2.0 * tick_us + 500 + 1000) / 1000.0; }

std::vector<std::string> tx_latency_presets(unsigned tick_us) {
  const double min = min_tx_latency_ms(tick_us);
  std::vector<std::string> out{g_text(min)};
  for (double v : {3.0, 3.5, 4.0, 5.0, 6.0, 10.0, 20.0, 40.0})
    if (v > min + 1e-9) out.push_back(g_text(v));
  return out;
}

std::string tx_latency_min_text(unsigned tick_us) { return "(" + g_text(min_tx_latency_ms(tick_us)) + " ms minimum)"; }

std::string tx_latency_problem(const SettingsForm& f) {
  char* end = nullptr;
  const double v = std::strtod(f.tx_latency_ms.c_str(), &end);
  if (end == f.tx_latency_ms.c_str() || !(v > 0)) return "Enter the transmit latency in milliseconds, for example 4.";
  const double min = min_tx_latency_ms(f.tick_us);
  if (v + 1e-9 < min)
    return "The transmit latency must be at least " + g_text(min) + " ms with a " + g_text(f.tick_us / 1000.0) +
           " ms mixing tick.";
  if (v > 200 + 1e-9) return "The transmit latency can be at most 200 ms.";
  return "";
}

std::string tx_latency_warning(const SettingsForm& f) {
  if (!(std::strtod(f.tx_latency_ms.c_str(), nullptr) > 40 + 1e-9)) return "";
  return "Above 40 ms is experimental: the receiving Dante devices must buffer audio that arrives this early, and "
         "some drop it. Test with your devices.";
}

std::string tx_suggestion_note(const TxSuggestion& s, unsigned form_tick_us) {
  if (!s.present || s.text.empty()) return "";
  std::string t = s.text;
  if (s.tick_us && form_tick_us != s.tick_us)
    return t + " Measured at the " + g_text(s.tick_us / 1000.0) +
           " ms tick: measuring starts again once the new tick is applied.";
  if (s.actionable()) t += " Dante receivers keep their own latency setting.";
  return t;
}

bool tx_suggestion_applicable(const TxSuggestion& s, unsigned form_tick_us) {
  return s.actionable() && (!s.tick_us || s.tick_us == form_tick_us);
}

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
  f.tick_us = conf_tick(c);
  f.local_clock = lower(conf_get(c, "device", "clock")) == "free";
  f.master_capable = truthy(conf_get(c, "ptp", "master_capable"));
  f.wifi = truthy(conf_get(c, "ptp", "wifi"));
  f.remote_access = truthy(conf_get(c, "device", "remote_access"));
  f.remote_password = conf_get(c, "device", "remote_password");
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
  // Only written when it changes, so a file without it stays without it.
  if (f.tick_us != conf_tick(conf)) c = conf_set(c, "device", "tick_us", std::to_string(f.tick_us));
  c = conf_set(c, "device", "clock", f.local_clock ? "free" : "ptp");
  c = conf_set(c, "ptp", "master_capable", f.master_capable ? "true" : "false");
  c = conf_set(c, "ptp", "wifi", f.wifi ? "true" : "false");
  // Written only when used, so files that never had them stay as they are.
  if (f.remote_access || !conf_get(conf, "device", "remote_access").empty())
    c = conf_set(c, "device", "remote_access", f.remote_access ? "true" : "false");
  if (!f.remote_password.empty()) c = conf_set(c, "device", "remote_password", f.remote_password);
  else c = conf_remove(c, "device", "remote_password");
  return c;
}

std::string remote_access_problem(const SettingsForm& f) {
  for (char ch : f.remote_password)
    if (ch == '"' || static_cast<unsigned char>(ch) < 0x20) return "The password cannot contain quotes.";
  if (f.remote_access && f.remote_password.size() < 8)
    return "Remote access needs a password of at least 8 characters.";
  return "";
}

bool save_settings(unsigned port, const SettingsForm& f, std::string* error) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    ConfigText fresh;
    if (!fetch_config(port, &fresh, error)) return false;
    const SaveResult r = save_config_text(port, apply_settings(fresh.text, f), fresh.version);
    if (r.ok) return true;
    if (r.conflict) continue;  // changed meanwhile: apply again
    if (error) *error = r.error;
    return false;
  }
  if (error) *error = "The configuration keeps changing elsewhere; try again.";
  return false;
}

}  // namespace vc
