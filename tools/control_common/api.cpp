#include "api.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace vc {

// ---- JSON -----------------------------------------------------------------------

static const Json kNull;

const Json& Json::operator[](const char* key) const {
  if (type != Object) return kNull;
  for (const auto& kv : o)
    if (kv.first == key) return kv.second;
  return kNull;
}
const Json& Json::operator[](size_t i) const { return type == Array && i < a.size() ? a[i] : kNull; }

namespace {
struct Parser {
  const char* p;
  const char* end;
  void ws() {
    while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
  }
  bool lit(const char* w) {
    const size_t n = std::strlen(w);
    if (size_t(end - p) < n || std::strncmp(p, w, n) != 0) return false;
    p += n;
    return true;
  }
  bool string(std::string* out) {
    if (p >= end || *p != '"') return false;
    ++p;
    while (p < end && *p != '"') {
      char c = *p++;
      if (c == '\\' && p < end) {
        c = *p++;
        switch (c) {
          case 'n': out->push_back('\n'); break;
          case 't': out->push_back('\t'); break;
          case 'r': out->push_back('\r'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'u': {
            if (end - p < 4) return false;
            unsigned cp = unsigned(std::strtoul(std::string(p, 4).c_str(), nullptr, 16));
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
              const unsigned lo = unsigned(std::strtoul(std::string(p + 2, 4).c_str(), nullptr, 16));
              if (lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
              }
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;  // lone surrogate
            if (cp < 0x80) {
              out->push_back(char(cp));
            } else if (cp < 0x800) {
              out->push_back(char(0xC0 | (cp >> 6)));
              out->push_back(char(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
              out->push_back(char(0xE0 | (cp >> 12)));
              out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
              out->push_back(char(0x80 | (cp & 0x3F)));
            } else {
              out->push_back(char(0xF0 | (cp >> 18)));
              out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
              out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
              out->push_back(char(0x80 | (cp & 0x3F)));
            }
            break;
          }
          default: out->push_back(c);
        }
      } else {
        out->push_back(c);
      }
    }
    if (p >= end) return false;
    ++p;
    return true;
  }
  bool value(Json* v) {
    ws();
    if (p >= end) return false;
    if (*p == '{') {
      ++p;
      v->type = Json::Object;
      ws();
      if (p < end && *p == '}') return ++p, true;
      for (;;) {
        ws();
        std::pair<std::string, Json> kv;
        if (!string(&kv.first)) return false;
        ws();
        if (p >= end || *p != ':') return false;
        ++p;
        if (!value(&kv.second)) return false;
        v->o.push_back(std::move(kv));
        ws();
        if (p < end && *p == ',') { ++p; continue; }
        if (p < end && *p == '}') return ++p, true;
        return false;
      }
    }
    if (*p == '[') {
      ++p;
      v->type = Json::Array;
      ws();
      if (p < end && *p == ']') return ++p, true;
      for (;;) {
        Json e;
        if (!value(&e)) return false;
        v->a.push_back(std::move(e));
        ws();
        if (p < end && *p == ',') { ++p; continue; }
        if (p < end && *p == ']') return ++p, true;
        return false;
      }
    }
    if (*p == '"') {
      v->type = Json::String;
      return string(&v->s);
    }
    if (lit("true")) return v->type = Json::Bool, v->b = true, true;
    if (lit("false")) return v->type = Json::Bool, v->b = false, true;
    if (lit("null")) return v->type = Json::Null, true;
    char* e = nullptr;
    v->n = std::strtod(p, &e);
    if (e == p) return false;
    p = e;
    v->type = Json::Number;
    return true;
  }
};
}  // namespace

bool parse_json(const std::string& text, Json* out) {
  Parser ps{text.data(), text.data() + text.size()};
  *out = Json();
  return ps.value(out);
}

// ---- HTTP -----------------------------------------------------------------------

HttpResult http(unsigned port, const char* method, const std::string& path,
                const std::string& body) {
  HttpResult r;
#if defined(_WIN32)
  static bool wsa = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  (void)wsa;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return r;
  DWORD tv = 1500;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
#else
  int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) return r;
  timeval tv{1, 500000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(static_cast<unsigned short>(port));
  sa.sin_addr.s_addr = htonl(0x7F000001);
  if (connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0) {
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                      std::to_string(port) + "\r\nConnection: close\r\nX-Virgil-Request: 1\r\n";
    if (!body.empty() || std::strcmp(method, "POST") == 0)
      req += "Content-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    size_t sent = 0;
    while (sent < req.size()) {
      const int n = send(s, req.data() + sent, int(req.size() - sent), 0);
      if (n <= 0) break;
      sent += size_t(n);
    }
    std::string resp;
    char buf[8192];
    for (;;) {
      const int n = recv(s, buf, sizeof buf, 0);
      if (n <= 0) break;
      resp.append(buf, size_t(n));
    }
    const size_t sp = resp.find(' ');
    const size_t hdr_end = resp.find("\r\n\r\n");
    if (resp.compare(0, 5, "HTTP/") == 0 && sp != std::string::npos && hdr_end != std::string::npos) {
      r.status = std::atoi(resp.c_str() + sp + 1);
      r.body = resp.substr(hdr_end + 4);
    }
  }
#if defined(_WIN32)
  closesocket(s);
#else
  close(s);
#endif
  return r;
}

// ---- config text editing (same rules as the daemon's parser) --------------------

namespace {
std::vector<std::string> split_lines(const std::string& t) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    const size_t b = t.find('\n', a);
    out.push_back(t.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}
std::string join_lines(const std::vector<std::string>& l) {
  std::string out;
  for (size_t i = 0; i < l.size(); ++i) {
    if (i) out += '\n';
    out += l[i];
  }
  return out;
}
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}
std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
// Section header name, or empty when the line is not a header.
std::string header_of(const std::string& line) {
  const std::string t = trim(line);
  if (t.size() >= 2 && t.front() == '[' && t.back() == ']') return lower(trim(t.substr(1, t.size() - 2)));
  return {};
}
// "key = value" -> key (lower case); empty if not a key line.
std::string key_of(const std::string& line, size_t* eq) {
  const size_t e = line.find('=');
  if (e == std::string::npos) return {};
  const std::string k = trim(line.substr(0, e));
  if (k.empty()) return {};
  for (char c : k)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return {};
  if (eq) *eq = e;
  return lower(k);
}
std::string quote(const std::string& v) {
  const bool need = v.find_first_of(";#") != std::string::npos ||
                    (!v.empty() && (std::isspace(static_cast<unsigned char>(v.front())) ||
                                    std::isspace(static_cast<unsigned char>(v.back()))));
  return need ? "\"" + v + "\"" : v;
}
}  // namespace

namespace {
// The daemon reads [device] and [network] alike, and some keys under a
// second name; the last occurrence of a key wins.
bool section_matches(const std::string& want, const std::string& cur) {
  if (want == "device") return cur == "device" || cur == "network";
  return cur == want;
}
bool key_matches(const std::string& want, const std::string& key, const std::string& section) {
  if (key == want) return true;
  if (section != "device") return false;
  static const char* const kAliases[][2] = {
      {"tx_channels", "playback_channels"}, {"rx_channels", "capture_channels"}, {"latency_us", "rx_latency_us"}};
  for (const auto& a : kAliases)
    if ((want == a[0] && key == a[1]) || (want == a[1] && key == a[0])) return true;
  return false;
}
}  // namespace

std::string conf_get(const std::string& text, const std::string& section, const std::string& key) {
  std::string cur, found;
  for (const auto& line : split_lines(text)) {
    const std::string h = header_of(line);
    if (!h.empty()) { cur = h; continue; }
    size_t eq = 0;
    if (section_matches(section, cur) && key_matches(key, key_of(line, &eq), section)) {
      std::string v, raw = line.substr(eq + 1);
      bool q = false;
      for (char c : raw) {
        if (c == '"') { q = !q; continue; }
        if (!q && (c == ';' || c == '#')) break;
        v += c;
      }
      found = trim(v);
    }
  }
  return found;
}

std::string conf_set(const std::string& text, const std::string& section, const std::string& key,
                     const std::string& value) {
  auto lines = split_lines(text);
  std::string cur;
  int sec_start = -1, sec_end = -1, alt_start = -1, alt_end = -1;
  bool replaced = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string h = header_of(lines[i]);
    if (!h.empty()) {
      if (cur == section && sec_end < 0 && sec_start >= 0) sec_end = int(i);
      if (cur != section && section_matches(section, cur) && alt_end < 0 && alt_start >= 0) alt_end = int(i);
      cur = h;
      if (h == section && sec_start < 0) sec_start = int(i);
      else if (h != section && section_matches(section, h) && alt_start < 0) alt_start = int(i);
      continue;
    }
    size_t eq = 0;
    // Every occurrence (also under the other name), so none overrides it.
    if (section_matches(section, cur) && key_matches(key, key_of(lines[i], &eq), section)) {
      lines[i] = trim(lines[i].substr(0, eq)) + " = " + quote(value);
      replaced = true;
    }
  }
  if (replaced) return join_lines(lines);
  const std::string line = key + " = " + quote(value);
  if (sec_start < 0 && alt_start >= 0) sec_start = alt_start, sec_end = alt_end;
  if (sec_start < 0) {
    std::string t = text;
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    return t + "\n\n[" + section + "]\n" + line + "\n";
  }
  int at = sec_end < 0 ? int(lines.size()) : sec_end;
  while (at > sec_start + 1 && trim(lines[size_t(at - 1)]).empty()) --at;
  lines.insert(lines.begin() + at, line);
  return join_lines(lines);
}

// ---- status -----------------------------------------------------------------------

std::string valid_utf8(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  const auto* p = reinterpret_cast<const unsigned char*>(in.data());
  const size_t n = in.size();
  for (size_t i = 0; i < n;) {
    const unsigned c = p[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    bool ok = len > 0 && i + len <= n;
    for (size_t k = 1; ok && k < len; ++k) ok = (p[i + k] & 0xC0) == 0x80;
    if (ok && len > 1) {
      unsigned cp = c & (0x7F >> len);
      for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (p[i + k] & 0x3F);
      static const unsigned kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
      ok = cp >= kMin[len] && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);  // no overlong / surrogates
    }
    if (ok) {
      out.append(in, i, len);
      i += len;
    } else {
      out += "\xEF\xBF\xBD";  // U+FFFD
      ++i;
    }
  }
  return out;
}

bool parse_status(const std::string& body, Status* st) {
  Json j;
  if (!parse_json(body, &j) || j.type != Json::Object) return false;
  Status s;
  s.reachable = true;
  s.version = j["version"].str();
  s.state = j["status"].str();
  s.error = j["error"].str();
  s.config_path = j["config_path"].str();
  const Json& d = j["device"];
  s.name = d["name"].str("Virgil");
  s.address = d["address"].str();
  s.iface = d["interface"].str();
  s.rate = unsigned(d["sample_rate"].num(48000));
  s.tx = unsigned(d["tx_channels"].num());
  s.rx = unsigned(d["rx_channels"].num());
  s.latency_us = unsigned(d["latency_us"].num());
  s.tx_latency_us = unsigned(d["tx_latency_us"].num());
  s.dante = d["dante"].boolean();
  const Json& c = j["clock"];
  s.clock = c["state"].str("stopped");
  s.clock_source = c["source"].str("ptp");
  s.offset_us = c["offset_us"].num();
  s.grandmaster = c["grandmaster"].str();
  const Json& pt = c["ptp"];
  if (pt.type == Json::Object) {
    s.ptp_info = true;
    s.ptp_senders = unsigned(std::max(0.0, pt["senders"].num()));
    s.ptp_master_heard = pt["master_heard"].boolean();
    s.no_leader_s = std::max(0.0, pt["no_leader_s"].num());
    s.ptp_adapter = pt["adapter"].str();
  }
  s.late_ticks = (unsigned long long)j["counters"]["late_ticks"].num();
  s.clock_steps = (unsigned long long)j["counters"]["clock_steps"].num();
  s.dropouts_last_hour = (unsigned long long)j["dropouts_last_hour"].num();
  for (size_t i = 0; i < j["tx_names"].size(); ++i) s.tx_names.push_back(j["tx_names"][i].str());
  for (size_t i = 0; i < j["rx_names"].size(); ++i) s.rx_names.push_back(j["rx_names"][i].str());
  const Json& cl = j["clients"];
  for (size_t i = 0; i < cl.size(); ++i) s.clients.push_back({cl[i]["name"].str(), cl[i]["active"].boolean()});
  const Json& m = j["meters"];
  for (size_t i = 0; i < m["tx"].size(); ++i) s.meter_tx.push_back(float(m["tx"][i].num()));
  for (size_t i = 0; i < m["rx"].size(); ++i) s.meter_rx.push_back(float(m["rx"][i].num()));
  // Shown by the UI toolkits, which may reject (or crash on) invalid UTF-8:
  // app names come from the apps, names and errors from the network.
  for (std::string* t : {&s.version, &s.state, &s.error, &s.config_path, &s.name, &s.address, &s.iface,
                         &s.clock, &s.clock_source, &s.grandmaster, &s.ptp_adapter})
    *t = valid_utf8(*t);
  for (auto* v : {&s.tx_names, &s.rx_names})
    for (auto& t : *v) t = valid_utf8(t);
  for (auto& cl : s.clients) cl.name = valid_utf8(cl.name);
  *st = std::move(s);
  return true;
}

ClockNotice clock_notice(const Status& s) {
  ClockNotice n;
  if (!s.reachable || s.state != "running" || !s.dante || s.clock_source != "ptp" || !s.ptp_info) return n;
  if (s.clock == "ptp-locked" || s.clock == "ptp-master" || s.clock == "stopped") return n;
  if (!(s.no_leader_s > kNoLeaderNoticeS)) return n;
  std::string where = !s.ptp_adapter.empty() ? s.ptp_adapter : !s.iface.empty() ? s.iface : "this adapter";
  if (!s.address.empty() && s.address != where) where += " (" + s.address + ")";
  const std::string become = "\xE2\x80\x9C" "Become clock leader when no Dante device provides one" "\xE2\x80\x9D";
  n.show = true;
  if (s.ptp_master_heard) {
    n.headline = "Dante clock leader lost";
    n.title = "Dante clock leader lost on " + where;
    n.hints = {"Check that the clock leader device is still on and connected.",
               "If it is gone, let another device lead or turn on " + become + "."};
  } else if (s.ptp_senders > 0) {
    n.headline = "No Dante clock leader yet";
    n.title = "Dante devices found on " + where + ", but none is the clock leader yet";
    n.hints = {"If none of them can lead, turn on " + become + ".",
               "Otherwise check that no firewall blocks UDP ports 319 and 320."};
  } else {
    n.headline = "No Dante clock leader";
    n.title = "No Dante clock leader found on " + where;
    n.hints = {"The Dante network may be on another adapter: Settings \xE2\x80\xBA Network interface.",
               "A firewall may block the Dante clock: allow UDP ports 319 and 320.",
               "No device here can lead the clock? Turn on " + become + "."};
  }
  return n;
}

std::vector<Interface> parse_interfaces(const std::string& body) {
  std::vector<Interface> out;
  Json j;
  if (!parse_json(body, &j)) return out;
  for (size_t i = 0; i < j.size(); ++i)
    out.push_back({j[i]["name"].str(), j[i]["address"].str(), j[i]["loopback"].boolean(),
                   j[i]["virtual"].boolean()});
  return out;
}

}  // namespace vc
