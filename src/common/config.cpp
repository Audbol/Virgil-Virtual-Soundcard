#include "virgil/config.h"
#include "virgil/shm_layout.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <cstdio>
#include <mutex>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef interface
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "virgil/platform.h"

namespace virgil {

const char* const kStockDeviceName = "Virgil";

static std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(uint8_t(s[b]))) ++b;
  while (e > b && std::isspace(uint8_t(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// The line without its comment (';' or '#', not inside a quoted value).
static std::string strip_comment(const std::string& line) {
  bool in_quotes = false;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"') in_quotes = !in_quotes;
    else if (!in_quotes && (line[i] == ';' || line[i] == '#')) return line.substr(0, i);
  }
  return line;
}

// Editors (Virgil Control, Dante Controller renames, Stop/Start) and the
// control panel's save run on different threads: one file write at a time.
static std::mutex g_file_mutex;
std::mutex& config_file_mutex() { return g_file_mutex; }

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
    // A UTF-8 byte order mark (Notepad "UTF-8 with BOM", PowerShell 5).
    if (lineno == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
    // Strip comments, but not ';' / '#' inside a quoted value.
    line = trim(strip_comment(line));
    if (line.empty()) continue;
    if (line.front() == '[') {
      if (line.back() != ']') return fail("bad section header");
      section = lower(trim(line.substr(1, line.size() - 2)));
      if (section == "tx" || section == "rx") {
        if (!noted_flows)
          c->notes.push_back("ignoring [tx]/[rx] sections from the old AES67 version: "
                             "route channels in Dante Controller instead");
        noted_flows = true;
      } else if (section != "device" && section != "ptp" && section != "network" &&
                 section != "channel_names") {
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
      else if (key == "remote_access") { if (!need_bool(&c->remote_access)) return false; }
      else if (key == "remote_password") c->remote_password = val;
      else if (key == "control_port") { if (!to_u32(val, &u) || u > 65535) return fail("bad control_port"); c->control_port = u; }
      else if (key == "clock") c->clock = lower(val);
      else if (key == "ports") c->ports = lower(val);
      else if (key == "enabled") { if (!need_bool(&c->enabled)) return false; }
      else if (key == "packet_time_us" || key == "sap") legacy();
      else return fail("unknown key '" + key + "'");
    } else if (section == "channel_names") {
      // tx1 = Kick, rx12 = Talkback
      const bool tx = key.rfind("tx", 0) == 0, rx = key.rfind("rx", 0) == 0;
      uint32_t n = 0;
      if ((!tx && !rx) || !to_u32(key.substr(2), &n) || n < 1 || n > 1024)
        return fail("channel names are written tx1 = ... or rx1 = ...");
      auto& v = tx ? c->tx_names : c->rx_names;
      if (v.size() < n) v.resize(n);
      v[n - 1] = val;
    } else if (section == "ptp") {
      if (key == "subdomain") c->ptp_subdomain = val;
      else if (key == "master_capable") { if (!need_bool(&c->ptp_master_capable)) return false; }
      else if (key == "wifi") { if (!need_bool(&c->ptp_wifi)) return false; }
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
  if (c->tx_channels > kMaxChannels || c->rx_channels > kMaxChannels)
    return fail("at most " + std::to_string(kMaxChannels) + " channels per direction");
  if (c->tx_channels + c->rx_channels == 0) return fail("needs at least one channel");
  if (c->device_name.empty() || c->device_name.size() > 31)
    return fail("name must be 1 to 31 characters");
  for (char ch : c->device_name)
    if (!(std::isalnum(uint8_t(ch)) || ch == '-' || ch == ' ' || ch == '_'))
      return fail("name may only contain letters, digits, spaces, '-' and '_'");
  if (c->tick_us < 125 || c->tick_us > 4000) return fail("tick_us must be between 125 and 4000");
  // Latency messages in ms, as Virgil Control and the panel show latency.
  auto ms = [](uint32_t us) {
    char b[32];
    std::snprintf(b, sizeof b, "%g ms", us / 1000.0);
    return std::string(b);
  };
  if (c->latency_us < 500 || c->latency_us > kMaxLatencyUs)
    return fail("receive latency (latency_us) must be between 0.5 ms and 200 ms");
  if (c->tx_latency_us < 500 || c->tx_latency_us > kMaxLatencyUs)
    return fail("transmit latency (tx_latency_us) must be between 0.5 ms and 200 ms");
  if (c->tx_latency_us < c->min_tx_latency_us())
    return fail("transmit latency (tx_latency_us) must be at least " + ms(c->min_tx_latency_us()) +
                " (two ticks + 1.5 ms)");
  if (c->clock != "ptp" && c->clock != "free") return fail("clock must be 'ptp' or 'free'");
  if (c->ports != "auto" && c->ports != "standard") {
    uint32_t p = 0;
    if (!to_u32(c->ports, &p) || p < 1024 || p > 65500)
      return fail("ports must be auto, standard or a port number between 1024 and 65500");
  }
  if (c->remote_access && c->remote_password.size() < 8)
    return fail(c->remote_password.empty()
                    ? "remote access needs a password: set remote_password (at least 8 characters)"
                    : "remote_password must be at least 8 characters");
  for (char ch : c->remote_password)
    if (ch == '"' || uint8_t(ch) < 0x20) return fail("remote_password cannot contain quotes or control characters");
  if (c->remote_access && c->control_port == 0)
    return fail("remote access needs the web panel: control_port cannot be 0");
  if (c->ptp_subdomain.empty() || c->ptp_subdomain.size() > 15)
    return fail("ptp subdomain must be 1 to 15 characters");

  // Channel names: valid, and unique per direction (Dante subscribes by name).
  for (int dir = 0; dir < 2; ++dir) {
    auto& v = dir == 0 ? c->tx_names : c->rx_names;
    const uint32_t n = dir == 0 ? c->tx_channels : c->rx_channels;
    if (v.size() > n) v.resize(n);  // names of channels that no longer exist
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i].empty()) continue;
      const std::string p = channel_name_problem(v[i]);
      const std::string which = std::string(dir == 0 ? "transmit" : "receive") + " channel " + std::to_string(i + 1);
      if (!p.empty()) return fail(which + ": " + p);
      for (size_t j = 0; j < n; ++j) {
        if (j == i) continue;
        const std::string other = j < v.size() && !v[j].empty() ? v[j] : default_channel_name(dir == 0, uint32_t(j + 1));
        if (lower(other) == lower(v[i])) return fail(which + ": the name '" + v[i] + "' is used twice");
      }
    }
  }

  if (c->tx_lead_us > 40000) return fail("tx_lead_us must be at most 40000");
  if (c->spin_us >= c->tick_us) return fail("spin_us must be smaller than tick_us");

  // Ring: comfortably larger than every latency in play (all bounded above,
  // so this stays far below 2^20 frames). The drivers' shared ring does not
  // hold the Dante latencies themselves (Inferno applies them in its own
  // rings, see dante_*_ring_frames), so latencies past 40 ms (Wi-Fi) count
  // as 40 ms here: the shared memory stays the size it was.
  const auto capped = [c](uint32_t us) { return uint64_t(c->us_to_frames(std::min<uint32_t>(us, 40000))); };
  const uint64_t need = 8ull * (capped(c->latency_us) + capped(c->tx_latency_us) + c->tx_lead_frames() +
                                4ull * c->period_frames());
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

namespace {
uint32_t pow2_at_least(uint64_t n, uint32_t floor) {
  uint32_t r = floor;
  while (r < n && r < (1u << 24)) r <<= 1;
  return r;
}
}  // namespace

// Inferno writes received audio at its media time + the receive latency, up
// to that far ahead of the engine reading it, plus however early senders
// stamp their packets (another Virgil: up to its transmit latency, counted
// as 40 ms). The engine silences half a ring ahead, so the ring is 4x that.
uint32_t Config::dante_rx_ring_frames() const {
  if (latency_us <= 40000) return ring_frames;  // ring_frames covers it
  return pow2_at_least(4ull * (uint64_t(us_to_frames(latency_us)) + us_to_frames(40000) + 4ull * period_frames()),
                       ring_frames);
}

// Inferno sends samples up to the transmit latency late (after a stall) and
// reads them by media time; the engine writes a tick ahead.
uint32_t Config::dante_tx_ring_frames() const {
  if (tx_latency_us <= 40000) return ring_frames;
  return pow2_at_least(4ull * (uint64_t(us_to_frames(tx_latency_us)) + tx_lead_frames() + 4ull * period_frames()),
                       ring_frames);
}

std::vector<std::string> device_channel_names(const Config& c, bool tx) {
  bool any = false;
  for (const auto* v : {&c.tx_names, &c.rx_names})
    for (const auto& n : *v) any |= !n.empty();
  const uint32_t n = tx ? c.tx_channels : c.rx_channels;
  std::vector<std::string> out(n);
  if (!any) return out;
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = tx ? c.tx_name(i) : c.rx_name(i);
    if (out[i].empty()) out[i] = default_channel_name(tx, i + 1);
  }
  return out;
}

static bool replace_file(const std::string& from, const std::string& to);

// Written to disk before it replaces the old file, which is replaced in one
// step: a crash or power cut leaves either the old or the new file. On POSIX
// the new file keeps the old one's mode and owner (macOS: 664 root:admin so
// administrators can edit it). Hold config_file_mutex().
bool write_config_file(const std::string& path, const std::string& text, std::string* error) {
  const std::string tmp = path + ".new";
#if defined(_WIN32)
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    o << text;
    o.flush();
    if (!o) {
      if (error) *error = "cannot write " + tmp;
      std::remove(tmp.c_str());
      return false;
    }
  }
#else
  struct stat st{};
  const bool had = ::stat(path.c_str(), &st) == 0;
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, had ? (st.st_mode & 07777) : 0644);
  bool ok = fd >= 0;
  if (ok && had) {
    (void)::fchmod(fd, st.st_mode & 07777);  // regardless of the umask
    if (::fchown(fd, st.st_uid, st.st_gid) != 0) {
      // Only root may give a file away; the service owns its directory anyway.
    }
  }
  for (size_t off = 0; ok && off < text.size();) {
    const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
    if (n <= 0) ok = false;
    else off += size_t(n);
  }
  if (ok) ok = ::fsync(fd) == 0;
  if (fd >= 0 && ::close(fd) != 0) ok = false;
  if (!ok) {
    if (error) *error = "cannot write " + tmp;
    std::remove(tmp.c_str());
    return false;
  }
#endif
  if (!replace_file(tmp, path)) {
    if (error) *error = "cannot replace " + path;
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

std::string channel_name_problem(const std::string& name) {
  if (name.empty()) return "the name is empty";
  if (name.size() > 31) return "names can be at most 31 characters";
  if (name.front() == ' ' || name.back() == ' ') return "names cannot start or end with a space";
  for (char ch : name) {
    const auto u = uint8_t(ch);
    if (u < 0x20 || u > 0x7e) return "names may only use letters, digits, spaces and common punctuation";
    if (ch == '=' || ch == '.' || ch == '@' || ch == '"' || ch == '\\')
      return std::string("names cannot contain '") + ch + "'";
  }
  return "";
}

std::string default_channel_name(bool tx, uint32_t number) {
  return std::string(tx ? "TX " : "RX ") + std::to_string(number);
}

static bool replace_file(const std::string& from, const std::string& to) {
#if defined(_WIN32)
  return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

std::string host_device_name() {
  std::string h;
  for (char ch : host_name()) {
    if (h.size() >= 24) break;
    h += std::isalnum(uint8_t(ch)) || ch == '-' ? ch : '-';
  }
  while (!h.empty() && h.back() == '-') h.pop_back();
  return h.empty() ? kStockDeviceName : std::string(kStockDeviceName) + "-" + h;
}

bool replace_stock_name(const std::string& path, const std::string& name, std::string* error) {
  std::lock_guard<std::mutex> lock(g_file_mutex);
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  const std::string text = ss.str();
  std::string out, section;
  bool changed = false;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    end = end == std::string::npos ? text.size() : end + 1;
    std::string line = text.substr(pos, end - pos);
    pos = end;
    const std::string t = trim(strip_comment(line));
    if (!t.empty() && t.front() == '[' && t.back() == ']') section = lower(trim(t.substr(1, t.size() - 2)));
    const size_t eq = t.find('=');
    if (!changed && (section.empty() || section == "device" || section == "network") && eq != std::string::npos &&
        lower(trim(t.substr(0, eq))) == "name") {
      std::string v = trim(t.substr(eq + 1));
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      if (v == kStockDeviceName) {
        const bool crlf = line.size() >= 2 && line[line.size() - 2] == '\r';
        line = "name = " + name + (crlf ? "\r\n" : "\n");
        changed = true;
      }
    }
    out += line;
  }
  if (!changed) return false;
  return write_config_file(path, out, error);
}

bool set_config_value(const std::string& path, const std::string& section_name, const std::string& key,
                      const std::string& value, std::string* error) {
  std::lock_guard<std::mutex> lock(g_file_mutex);
  std::string text;
  {
    std::ifstream in(path, std::ios::binary);
    if (in) {
      std::stringstream ss;
      ss << in.rdbuf();
      text = ss.str();
    }
  }
  // The value is written quoted on one line: never let it end the line or
  // the quotes (it would add keys or sections of its own).
  if (value.find_first_of("\r\n\"") != std::string::npos) {
    if (error) *error = "value contains a line break or a quote";
    return false;
  }
  const bool crlf = text.find("\r\n") != std::string::npos;
  const std::string nl = crlf ? "\r\n" : "\n";
  const std::string entry = value.empty() ? std::string() : key + " = \"" + value + "\"" + nl;
  // Find the section and the key inside it.
  std::string out, section;
  bool in_section = false, done = false, have_section = false;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    end = end == std::string::npos ? text.size() : end + 1;
    std::string line = text.substr(pos, end - pos);
    pos = end;
    const std::string t = trim(strip_comment(line));
    if (!t.empty() && t.front() == '[' && t.back() == ']') {
      if (in_section && !done) {  // end of the section: add the key there
        out += entry;
        done = true;
      }
      section = lower(trim(t.substr(1, t.size() - 2)));
      in_section = section == section_name;
      have_section |= in_section;
    } else if (in_section) {
      const size_t eq = t.find('=');
      if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == key) {
        // Replace the first (or drop it, for an empty value); drop later
        // copies, which would otherwise win when the file is read.
        if (!done) out += entry;
        done = true;
        continue;
      }
    }
    out += line;
  }
  if (!done && !entry.empty()) {
    if (!out.empty() && out.back() != '\n') out += nl;
    if (!have_section) out += nl + "[" + section_name + "]" + nl;
    out += entry;
  }
  return write_config_file(path, out, error);
}

bool save_channel_name(const std::string& path, bool tx, uint32_t index, const std::string& name,
                       std::string* error) {
  return set_config_value(path, "channel_names", std::string(tx ? "tx" : "rx") + std::to_string(index + 1), name,
                          error);
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
  if (c.ports != "auto") o << "ports = " << c.ports << "\n";
  if (!c.enabled) o << "enabled = false\n";
  o << "control_port = " << c.control_port << "\n";
  if (c.remote_access) o << "remote_access = true\n";
  if (!c.remote_password.empty()) o << "remote_password = \"" << c.remote_password << "\"\n";
  if (c.tick_us != 1000) o << "tick_us = " << c.tick_us << "\n";
  if (c.tx_lead_us) o << "tx_lead_us = " << c.tx_lead_us << "\n";
  o << "rt_priority = " << c.rt_priority << "\n";
  o << "spin_us = " << c.spin_us << "\n";
  o << "lock_memory = " << (c.lock_memory ? "true" : "false") << "\n";
  o << "\n[ptp]\nsubdomain = " << c.ptp_subdomain << "\n";
  o << "master_capable = " << (c.ptp_master_capable ? "true" : "false") << "\n";
  if (c.ptp_wifi) o << "wifi = true\n";
  bool names = false;
  for (int dir = 0; dir < 2; ++dir) {
    const auto& v = dir == 0 ? c.tx_names : c.rx_names;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i].empty()) continue;
      if (!names) o << "\n[channel_names]\n";
      names = true;
      o << (dir == 0 ? "tx" : "rx") << (i + 1) << " = \"" << v[i] << "\"\n";
    }
  }
  return o.str();
}

}  // namespace virgil
