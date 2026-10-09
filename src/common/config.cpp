#include "virgil/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace virgil {

static std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(uint8_t(s[b]))) ++b;
  while (e > b && std::isspace(uint8_t(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

static std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

static bool to_u32(const std::string& v, uint32_t* out) {
  try {
    size_t pos;
    unsigned long x = std::stoul(v, &pos, 10);
    if (pos != v.size() || x > 0xffffffffUL) return false;
    *out = uint32_t(x);
    return true;
  } catch (...) {
    return false;
  }
}

static bool to_bool(const std::string& v, bool* out) {
  const std::string l = lower(v);
  if (l == "1" || l == "true" || l == "yes" || l == "on") return *out = true, true;
  if (l == "0" || l == "false" || l == "no" || l == "off") return *out = false, true;
  return false;
}

bool parse_config(const std::string& text, Config* c, std::string* error) {
  std::istringstream in(text);
  std::string line, section;
  int lineno = 0;
  bool noted_flows = false;
  auto fail = [&](const std::string& msg) {
    if (error) *error = "line " + std::to_string(lineno) + ": " + msg;
    return false;
  };
  while (std::getline(in, line)) {
    ++lineno;
    // Strip comments, but not ';' / '#' inside a quoted value.
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"') in_quotes = !in_quotes;
      else if (!in_quotes && (line[i] == ';' || line[i] == '#')) {
        line.resize(i);
        break;
      }
    }
    line = trim(line);
    if (line.empty()) continue;
    if (line.front() == '[') {
      if (line.back() != ']') return fail("bad section header");
      section = lower(trim(line.substr(1, line.size() - 2)));
      if (section == "tx" || section == "rx") {
        if (!noted_flows)
          c->notes.push_back("ignoring [tx]/[rx] sections from the old AES67 version: "
                             "route channels in Dante Controller instead");
        noted_flows = true;
      } else if (section != "device" && section != "ptp" && section != "network") {
        return fail("unknown section [" + section + "]");
      }
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return fail("expected key = value");
    const std::string key = lower(trim(line.substr(0, eq)));
    std::string val = trim(line.substr(eq + 1));
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
      val = val.substr(1, val.size() - 2);

    uint32_t u = 0;
    bool b = false;
    auto need_u32 = [&](uint32_t* dst) {
      if (!to_u32(val, &u)) return fail("'" + key + "' needs an unsigned integer");
      *dst = u;
      return true;
    };
    auto need_bool = [&](bool* dst) {
      if (!to_bool(val, &b)) return fail("'" + key + "' needs true/false");
      *dst = b;
      return true;
    };
    auto legacy = [&]() {
      c->notes.push_back("ignoring '" + key + "' (AES67 setting, no longer used)");
      return true;
    };

    if (section == "tx" || section == "rx") continue;
    if (section == "device" || section == "network") {
      if (key == "name") c->device_name = val;
      else if (key == "interface") c->interface = val;
      else if (key == "shm_name") c->shm_name = val;
      else if (key == "sample_rate") { if (!need_u32(&c->sample_rate)) return false; }
      else if (key == "tx_channels" || key == "playback_channels") { if (!need_u32(&c->tx_channels)) return false; }
      else if (key == "rx_channels" || key == "capture_channels") { if (!need_u32(&c->rx_channels)) return false; }
      else if (key == "latency_us" || key == "rx_latency_us") { if (!need_u32(&c->latency_us)) return false; }
      else if (key == "tx_latency_us") { if (!need_u32(&c->tx_latency_us)) return false; }
      else if (key == "tick_us") { if (!need_u32(&c->tick_us)) return false; }
      else if (key == "tx_lead_us") { if (!need_u32(&c->tx_lead_us)) return false; }
      else if (key == "ring_frames") { if (!need_u32(&c->ring_frames)) return false; }
      else if (key == "rt_priority") { uint32_t p; if (!need_u32(&p)) return false; c->rt_priority = int(p); }
      else if (key == "spin_us") { if (!need_u32(&c->spin_us)) return false; }
      else if (key == "lock_memory") { if (!need_bool(&c->lock_memory)) return false; }
      else if (key == "control_port") { if (!to_u32(val, &u) || u > 65535) return fail("bad control_port"); c->control_port = u; }
      else if (key == "clock") c->clock = lower(val);
      else if (key == "packet_time_us" || key == "sap") legacy();
      else return fail("unknown key '" + key + "'");
    } else if (section == "ptp") {
      if (key == "subdomain") c->ptp_subdomain = val;
      else if (key == "master_capable") { if (!need_bool(&c->ptp_master_capable)) return false; }
      else if (key == "domain" || key == "priority1") legacy();
      else return fail("unknown key '" + key + "'");
    } else {
      return fail("key outside of a section");
    }
  }
  return true;
}

bool load_config(const std::string& path, Config* c, std::string* error) {
  std::ifstream f(path);
  if (!f) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return parse_config(ss.str(), c, error);
}

bool validate_config(Config* c, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  static const uint32_t rates[] = {44100, 48000, 88200, 96000};
  if (std::find(std::begin(rates), std::end(rates), c->sample_rate) == std::end(rates))
    return fail("sample_rate must be 44100, 48000, 88200 or 96000");
  if (c->tx_channels > 64 || c->rx_channels > 64) return fail("at most 64 channels per direction");
  if (c->tx_channels + c->rx_channels == 0) return fail("needs at least one channel");
  if (c->device_name.empty() || c->device_name.size() > 31)
    return fail("name must be 1 to 31 characters");
  for (char ch : c->device_name)
    if (!(std::isalnum(uint8_t(ch)) || ch == '-' || ch == ' ' || ch == '_'))
      return fail("name may only contain letters, digits, spaces, '-' and '_'");
  if (c->tick_us < 125 || c->tick_us > 4000) return fail("tick_us must be between 125 and 4000");
  if (c->latency_us < 500 || c->latency_us > 40000)
    return fail("latency_us must be between 500 and 40000");
  if (c->tx_latency_us < 500 || c->tx_latency_us > 40000)
    return fail("tx_latency_us must be between 500 and 40000");
  if (c->tx_latency_us < c->tx_send_delay_us() + 1000)
    return fail("tx_latency_us must be at least two ticks + 1500 (" +
                std::to_string(c->tx_send_delay_us() + 1000) + ")");
  if (c->clock != "ptp" && c->clock != "free") return fail("clock must be 'ptp' or 'free'");
  if (c->ptp_subdomain.empty() || c->ptp_subdomain.size() > 15)
    return fail("ptp subdomain must be 1 to 15 characters");

  if (c->tx_lead_us > 40000) return fail("tx_lead_us must be at most 40000");
  if (c->spin_us >= c->tick_us) return fail("spin_us must be smaller than tick_us");

  // Ring: comfortably larger than every latency in play (all bounded above,
  // so this stays far below 2^20 frames).
  const uint64_t need = 8ull * (uint64_t(c->us_to_frames(c->latency_us)) + c->us_to_frames(c->tx_latency_us) +
                                c->tx_lead_frames() + 4ull * c->period_frames());
  const uint32_t kMaxRing = 1u << 20;
  if (c->ring_frames == 0) {
    uint32_t r = 8192;
    while (r < need && r < kMaxRing) r <<= 1;
    c->ring_frames = r;
  }
  if (c->ring_frames & (c->ring_frames - 1)) return fail("ring_frames must be a power of two");
  if (c->ring_frames < need || c->ring_frames > kMaxRing)
    return fail("ring_frames must be a power of two between " + std::to_string(need) + " and " +
                std::to_string(kMaxRing));
  return true;
}

std::string format_config(const Config& c) {
  std::ostringstream o;
  auto quoted = [](const std::string& v) {
    const bool q = v.find_first_of(";#") != std::string::npos ||
                   (!v.empty() && (v.front() == ' ' || v.back() == ' '));
    return q ? "\"" + v + "\"" : v;
  };
  o << "# Virgil configuration\n\n[device]\n";
  o << "name = " << quoted(c.device_name) << "\n";
  o << "interface = " << quoted(c.interface) << "\n";
  if (!c.shm_name.empty()) o << "shm_name = " << c.shm_name << "\n";
  o << "sample_rate = " << c.sample_rate << "\n";
  o << "tx_channels = " << c.tx_channels << "\n";
  o << "rx_channels = " << c.rx_channels << "\n";
  o << "latency_us = " << c.latency_us << "\n";
  o << "tx_latency_us = " << c.tx_latency_us << "\n";
  o << "clock = " << c.clock << "\n";
  o << "control_port = " << c.control_port << "\n";
  if (c.tick_us != 1000) o << "tick_us = " << c.tick_us << "\n";
  if (c.tx_lead_us) o << "tx_lead_us = " << c.tx_lead_us << "\n";
  o << "rt_priority = " << c.rt_priority << "\n";
  o << "spin_us = " << c.spin_us << "\n";
  o << "lock_memory = " << (c.lock_memory ? "true" : "false") << "\n";
  o << "\n[ptp]\nsubdomain = " << c.ptp_subdomain << "\n";
  o << "master_capable = " << (c.ptp_master_capable ? "true" : "false") << "\n";
  return o.str();
}

}  // namespace virgil
