#include "config_model.h"

#include <cctype>
#include <cstdlib>

#include "api.h"

namespace vc {

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// As virgild reads a channel count; missing or unreadable is the default, 8.
unsigned channel_count(const std::string& v) {
  if (v.empty()) return 8;
  char* end = nullptr;
  const long n = std::strtol(v.c_str(), &end, 10);
  if (end == v.c_str() || *end || n < 0) return 8;
  return unsigned(n > 128 ? 128 : n);
}

std::string name_key(bool tx, unsigned index) { return std::string(tx ? "tx" : "rx") + std::to_string(index + 1); }

std::string at(const std::vector<std::string>& v, unsigned i) { return i < v.size() ? v[i] : std::string(); }

}  // namespace

// ---- configuration text ---------------------------------------------------------

bool fetch_config(unsigned port, ConfigText* out, std::string* error) {
  const HttpResult r = http(port, "GET", "/api/config");
  if (r.status != 200) {
    if (error) *error = http_error(r);
    return false;
  }
  if (out) {
    out->text = r.body;
    out->version = r.header("X-Virgil-Config-Version");
  }
  return true;
}

SaveResult save_config_text(unsigned port, const std::string& text, const std::string& base_version) {
  SaveResult s;
  const std::string hdr = base_version.empty() ? std::string() : "X-Virgil-Config-Version: " + base_version + "\r\n";
  const HttpResult r = http(port, "POST", "/api/config", text, hdr);
  if (r.status == 200) {
    Json j;
    parse_json(r.body, &j);
    s.ok = true;
    s.path = valid_utf8(j["path"].str());
    s.enabled = j["enabled"].boolean(true);
    return s;
  }
  s.conflict = r.status == 409;
  s.error = http_error(r);
  return s;
}

std::string saved_message(const SaveResult& r) {
  const std::string where = r.path.empty() ? "Saved" : "Saved to " + r.path;
  return where + (r.enabled ? ". Audio engine restarting\xE2\x80\xA6"
                            : ". Virgil is stopped: the settings take effect when it is started.");
}

// ---- channel names ----------------------------------------------------------------

std::string default_channel_name(bool tx, unsigned number) {
  return std::string(tx ? "TX " : "RX ") + std::to_string(number);
}

// Mirrors virgil::channel_name_problem (src/common/config.cpp); a unit test
// checks that both agree.
std::string channel_name_problem(const std::string& name) {
  if (name.empty()) return "the name is empty";
  if (name.size() > kMaxNameLength) return "names can be at most 31 characters";
  if (name.front() == ' ' || name.back() == ' ') return "names cannot start or end with a space";
  for (char ch : name) {
    const auto u = static_cast<unsigned char>(ch);
    if (u < 0x20 || u > 0x7e) return "names may only use letters, digits, spaces and common punctuation";
    if (ch == '=' || ch == '.' || ch == '@' || ch == '"' || ch == '\\')
      return std::string("names cannot contain '") + ch + "'";
  }
  return "";
}

std::string trim_name(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

ChannelNames load_channel_names(const std::string& conf) {
  ChannelNames n;
  n.tx = channel_count(conf_get(conf, "device", "tx_channels"));
  n.rx = channel_count(conf_get(conf, "device", "rx_channels"));
  for (int dir = 0; dir < 2; ++dir) {
    const bool tx = dir == 0;
    const unsigned count = tx ? n.tx : n.rx;
    auto& v = n.names(tx);
    v.resize(count);
    for (unsigned i = 0; i < count; ++i) v[i] = valid_utf8(conf_get(conf, "channel_names", name_key(tx, i)));
  }
  return n;
}

std::vector<NameError> validate_channel_names(const ChannelNames& n) {
  std::vector<NameError> out;
  for (int dir = 0; dir < 2; ++dir) {
    const bool tx = dir == 0;
    const unsigned count = tx ? n.tx : n.rx;
    const auto& v = n.names(tx);
    std::vector<std::string> eff(count);  // effective names, lower case
    for (unsigned i = 0; i < count; ++i) {
      const std::string t = trim_name(at(v, i));
      eff[i] = lower(t.empty() ? default_channel_name(tx, i + 1) : t);
    }
    for (unsigned i = 0; i < count; ++i) {
      const std::string t = trim_name(at(v, i));
      if (t.empty()) continue;  // the default: always fine
      std::string p = channel_name_problem(t);
      if (p.empty())
        for (unsigned j = 0; j < count && p.empty(); ++j)
          if (j != i && eff[j] == eff[i]) p = "also the name of channel " + std::to_string(j + 1);
      if (!p.empty()) out.push_back({tx, i, p});
    }
  }
  return out;
}

std::string name_error(const std::vector<NameError>& errors, bool tx, unsigned index) {
  for (const auto& e : errors)
    if (e.tx == tx && e.index == index) return e.message;
  return {};
}

std::string apply_channel_names(const std::string& conf, const ChannelNames& names, const ChannelNames* base) {
  std::string c = conf;
  for (int dir = 0; dir < 2; ++dir) {
    const bool tx = dir == 0;
    const unsigned count = tx ? names.tx : names.rx;
    for (unsigned i = 0; i < count; ++i) {
      const std::string v = trim_name(at(names.names(tx), i));
      if (base && v == trim_name(at(base->names(tx), i))) continue;  // not changed here
      const std::string key = name_key(tx, i);
      c = v.empty() ? conf_remove(c, "channel_names", key) : conf_set(c, "channel_names", key, v);
    }
  }
  return c;
}

bool save_channel_names(unsigned port, const ChannelNames& base, const ChannelNames& edited, std::string* error) {
  const auto errors = validate_channel_names(edited);
  if (!errors.empty()) {
    const NameError& e = errors.front();
    if (error)
      *error = std::string(e.tx ? "Transmit" : "Receive") + " channel " + std::to_string(e.index + 1) + ": " +
               e.message;
    return false;
  }
  for (int attempt = 0; attempt < 4; ++attempt) {
    ConfigText fresh;
    if (!fetch_config(port, &fresh, error)) return false;
    const std::string text = apply_channel_names(fresh.text, edited, &base);
    if (text == fresh.text) return true;  // nothing changed
    const SaveResult r = save_config_text(port, text, fresh.version);
    if (r.ok) return true;
    if (r.conflict) continue;  // changed meanwhile: apply again
    if (error) *error = r.error;
    return false;
  }
  if (error) *error = "The configuration keeps changing elsewhere; try again.";
  return false;
}

std::string channel_name_placeholder(const std::vector<std::string>& live, bool tx, unsigned index) {
  const std::string v = at(live, index);
  return v.empty() ? default_channel_name(tx, index + 1) : v;
}

}  // namespace vc
