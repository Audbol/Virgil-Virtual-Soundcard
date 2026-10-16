#include "remote_auth.h"

#include <cctype>
#include <iterator>
#include <vector>

#include "virgil/config.h"

namespace virgil {

const char* const kPasswordMask = "********";

bool is_loopback(uint32_t addr) { return (addr >> 24) == 127; }

bool constant_time_equal(const std::string& a, const std::string& b) {
  // Runs over the longer length either way; the result needs equal lengths.
  const size_t n = a.size() > b.size() ? a.size() : b.size();
  unsigned diff = unsigned(a.size() ^ b.size());
  for (size_t i = 0; i < n; ++i) {
    const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    diff |= unsigned(x ^ y);
  }
  return diff == 0;
}

namespace {

int b64(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// As parse_config reads lines: comments (not inside quotes) cut off.
std::string strip_comment(const std::string& line) {
  bool q = false;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"') q = !q;
    else if (!q && (line[i] == ';' || line[i] == '#')) return line.substr(0, i);
  }
  return line;
}

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

// Calls fn(line, value) for every remote_password line of [device] /
// [network]; fn returns the replacement line.
template <typename Fn>
std::string each_password_line(const std::string& text, Fn fn) {
  auto lines = split_lines(text);
  std::string section;
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string& line = lines[i];
    // The daemon's parser skips a UTF-8 byte order mark: so must the masking.
    const std::string body = i == 0 && line.compare(0, 3, "\xEF\xBB\xBF") == 0 ? line.substr(3) : line;
    const std::string t = trim(strip_comment(body));
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
      section = lower(trim(t.substr(1, t.size() - 2)));
      continue;
    }
    if (section != "device" && section != "network") continue;
    const size_t eq = t.find('=');
    if (eq == std::string::npos || lower(trim(t.substr(0, eq))) != "remote_password") continue;
    std::string v = trim(t.substr(eq + 1));
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    line = fn(line, v);
  }
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) out += '\n';
    out += lines[i];
  }
  return out;
}

std::string password_line(const std::string& old_line, const std::string& value) {
  const size_t indent = old_line.find_first_not_of(" \t");
  const bool cr = !old_line.empty() && old_line.back() == '\r';
  return old_line.substr(0, indent == std::string::npos ? 0 : indent) + "remote_password = \"" + value + "\"" +
         (cr ? "\r" : "");
}

}  // namespace

bool basic_auth_password(const std::string& authorization, std::string* password) {
  const std::string a = trim(authorization);
  if (a.size() < 6 || lower(a.substr(0, 6)) != "basic ") return false;
  const std::string enc = trim(a.substr(6));
  std::string dec;
  unsigned acc = 0;
  int bits = 0;
  size_t i = 0;
  for (; i < enc.size() && enc[i] != '='; ++i) {
    const int v = b64(enc[i]);
    if (v < 0) return false;
    acc = (acc << 6) | unsigned(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      dec.push_back(char((acc >> bits) & 0xFF));
    }
  }
  for (; i < enc.size(); ++i)
    if (enc[i] != '=') return false;
  const size_t colon = dec.find(':');
  if (colon == std::string::npos) return false;
  if (password) *password = dec.substr(colon + 1);
  return true;
}

RemoteAuth::Result RemoteAuth::check(uint32_t peer, bool remote_enabled, const std::string& password,
                                     const std::string& authorization, int64_t now) {
  if (is_loopback(peer)) return Result::Allowed;
  // A password the configuration rejects (under 8 characters, which keeps
  // the engine from starting) must not open the panel either.
  if (!remote_enabled || password.size() < 8) return Result::Refused;
  std::lock_guard<std::mutex> l(m_);
  // Forget addresses that have been quiet for a while (bounded memory).
  if (peers_.size() > 1024)
    for (auto it = peers_.begin(); it != peers_.end();)
      it = now - it->second.last > 10 * 60000 && now >= it->second.locked_until ? peers_.erase(it) : std::next(it);
  auto found = peers_.find(peer);
  if (found != peers_.end() && now < found->second.locked_until) return Result::LockedOut;
  std::string given;
  // No credentials yet: a browser asks for them first; not a failure.
  if (authorization.empty()) return Result::NeedLogin;
  const bool ok = basic_auth_password(authorization, &given) && constant_time_equal(given, password);
  if (ok) {
    if (found != peers_.end()) peers_.erase(found);
    return Result::Allowed;
  }
  Peer& p = peers_[peer];
  p.last = now;
  if (++p.failures >= kMaxFailures) {
    p.failures = 0;
    p.locked_until = now + kLockoutMs;
    return Result::LockedOut;
  }
  return Result::NeedLogin;
}

int RemoteAuth::retry_after_s(uint32_t peer, int64_t now) const {
  std::lock_guard<std::mutex> l(m_);
  auto it = peers_.find(peer);
  const int64_t ms = it == peers_.end() ? 0 : it->second.locked_until - now;
  return ms > 0 ? int((ms + 999) / 1000) : 1;
}

std::string mask_remote_password(const std::string& conf) {
  return each_password_line(conf, [](const std::string& line, const std::string& v) {
    return v.empty() ? line : password_line(line, kPasswordMask);
  });
}

bool resolve_posted_config(const std::string& posted, const std::string& current, bool local, std::string* out,
                           std::string* error) {
  Config cur;
  std::string ignored;
  if (!parse_config(current, &cur, &ignored)) cur = Config();
  // The mask stands for the password as it is.
  std::string text = each_password_line(posted, [&](const std::string& line, const std::string& v) {
    return v == kPasswordMask ? password_line(line, cur.remote_password) : line;
  });
  if (!local) {
    Config next;
    if (parse_config(text, &next, &ignored) &&
        (next.remote_access != cur.remote_access || !constant_time_equal(next.remote_password, cur.remote_password))) {
      if (error) *error = "remote access and its password can only be changed on the Virgil computer itself";
      return false;
    }
  }
  if (out) *out = text;
  return true;
}

}  // namespace virgil
