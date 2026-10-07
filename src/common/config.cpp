#include "dsv/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace dsv {

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
  auto fail = [&](const std::string& msg) {
    if (error) *error = "line " + std::to_string(lineno) + ": " + msg;
    return false;
  };
  while (std::getline(in, line)) {
    ++lineno;
    const size_t comment = line.find_first_of(";#");
    if (comment != std::string::npos) line.resize(comment);
    line = trim(line);
    if (line.empty()) continue;
    if (line.front() == '[') {
      if (line.back() != ']') return fail("bad section header");
      section = lower(trim(line.substr(1, line.size() - 2)));
      if (section == "tx") c->tx.emplace_back();
      else if (section == "rx") c->rx.emplace_back();
      else if (section != "device" && section != "ptp" && section != "network")
        return fail("unknown section [" + section + "]");
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

    if (section == "device" || section == "network") {
      if (key == "name") c->device_name = val;
      else if (key == "interface") c->interface = val;
      else if (key == "shm_name") c->shm_name = val;
      else if (key == "sample_rate") { if (!need_u32(&c->sample_rate)) return false; }
      else if (key == "tx_channels" || key == "playback_channels") { if (!need_u32(&c->tx_channels)) return false; }
      else if (key == "rx_channels" || key == "capture_channels") { if (!need_u32(&c->rx_channels)) return false; }
      else if (key == "packet_time_us") { if (!need_u32(&c->packet_time_us)) return false; }
      else if (key == "latency_us" || key == "rx_latency_us") { if (!need_u32(&c->rx_latency_us)) return false; }
      else if (key == "ring_frames") { if (!need_u32(&c->ring_frames)) return false; }
      else if (key == "rt_priority") { uint32_t p; if (!need_u32(&p)) return false; c->rt_priority = int(p); }
      else if (key == "spin_us") { if (!need_u32(&c->spin_us)) return false; }
      else if (key == "lock_memory") { if (!need_bool(&c->lock_memory)) return false; }
      else if (key == "sap") { if (!need_bool(&c->sap)) return false; }
      else if (key == "clock") c->clock = lower(val);
      else return fail("unknown key '" + key + "'");
    } else if (section == "ptp") {
      if (key == "domain") { if (!need_u32(&c->ptp_domain)) return false; }
      else if (key == "master_capable") { if (!need_bool(&c->ptp_master_capable)) return false; }
      else if (key == "priority1") { if (!need_u32(&c->ptp_priority1)) return false; }
      else return fail("unknown key '" + key + "'");
    } else if (section == "tx" || section == "rx") {
      StreamConfig& s = section == "tx" ? c->tx.back() : c->rx.back();
      if (key == "name") s.name = val;
      else if (key == "address") s.address = val;
      else if (key == "port") { if (!to_u32(val, &u) || u > 65535) return fail("bad port"); s.port = uint16_t(u); }
      else if (key == "first_channel") { if (!need_u32(&s.first_channel)) return false; }
      else if (key == "channels") { if (!need_u32(&s.channels)) return false; }
      else if (key == "payload_type") { if (!to_u32(val, &u) || u > 127) return fail("bad payload_type"); s.payload_type = uint8_t(u); }
      else if (key == "ttl") { if (!need_u32(&s.ttl)) return false; }
      else if (key == "encoding") s.encoding = val;
      else if (key == "sap_name" || key == "session") s.sap_name = val;
      else if (key == "source") s.source = val;
      else if (key == "ts_offset") { if (!need_u32(&s.ts_offset)) return false; }
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
  static const uint32_t rates[] = {44100, 48000, 88200, 96000, 176400, 192000};
  if (std::find(std::begin(rates), std::end(rates), c->sample_rate) == std::end(rates))
    return fail("sample_rate must be one of 44100/48000/88200/96000/176400/192000");
  if (c->tx_channels > 64 || c->rx_channels > 64) return fail("at most 64 channels per direction");
  if (c->packet_time_us < 125 || c->packet_time_us > 4000)
    return fail("packet_time_us must be between 125 and 4000");
  if (c->period_frames() == 0) return fail("packet time too short for sample rate");
  if (c->clock != "ptp" && c->clock != "free") return fail("clock must be 'ptp' or 'free'");
  if (c->ptp_domain > 127) return fail("ptp domain must be 0..127");

  const uint32_t min_latency = 2 * c->packet_time_us;
  if (c->rx_latency_us < min_latency) c->rx_latency_us = min_latency;

  // Auto ring size: comfortably larger than latency + packet slack both ways.
  if (c->ring_frames == 0) {
    uint32_t need = 8 * (c->rx_latency_frames() + 4 * c->period_frames());
    uint32_t r = 4096;
    while (r < need) r <<= 1;
    c->ring_frames = r;
  }
  if (c->ring_frames & (c->ring_frames - 1)) return fail("ring_frames must be a power of two");
  if (c->ring_frames < 8 * c->rx_latency_frames()) return fail("ring_frames too small for latency");

  auto check_streams = [&](std::vector<StreamConfig>& v, uint32_t dev_ch, const char* dir) {
    for (size_t i = 0; i < v.size(); ++i) {
      auto& s = v[i];
      const std::string where = std::string(dir) + " stream " + std::to_string(i + 1);
      if (s.channels == 0 || s.first_channel == 0 ||
          s.first_channel - 1 + s.channels > dev_ch)
        return fail(where + ": channels exceed the device's " + std::to_string(dev_ch));
      if (s.encoding != "L24" && s.encoding != "L16") return fail(where + ": encoding L24|L16");
      const uint32_t bytes = s.channels * (s.encoding == "L16" ? 2 : 3) * c->period_frames();
      if (std::string(dir) == "tx" && bytes > 1440)
        return fail(where + ": packet would exceed the 1500 byte MTU; use fewer channels per "
                            "stream or a shorter packet time");
      if (std::string(dir) == "tx" && s.address.empty()) return fail(where + ": address missing");
      if (std::string(dir) == "rx" && s.address.empty() && s.sap_name.empty() && s.port == 0)
        return fail(where + ": needs address/port or sap_name");
    }
    return true;
  };
  if (!check_streams(c->tx, c->tx_channels, "tx")) return false;
  if (!check_streams(c->rx, c->rx_channels, "rx")) return false;
  for (size_t i = 0; i < c->tx.size(); ++i)
    if (c->tx[i].name.empty())
      c->tx[i].name = c->device_name + " " + std::to_string(c->tx[i].first_channel) + "-" +
                      std::to_string(c->tx[i].first_channel + c->tx[i].channels - 1);
  return true;
}

}  // namespace dsv
