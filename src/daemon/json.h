// Tiny JSON writer for the control API. Values are written in order;
// `comma_` says whether the next key/value in the current container needs a
// separator.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace virgil {

class Json {
 public:
  Json& begin_obj() { pre(); out_ += '{'; comma_ = false; return *this; }
  Json& end_obj() { out_ += '}'; comma_ = true; return *this; }
  Json& begin_arr() { pre(); out_ += '['; comma_ = false; return *this; }
  Json& end_arr() { out_ += ']'; comma_ = true; return *this; }
  Json& key(const char* k) { pre(); quote(k); out_ += ':'; comma_ = false; return *this; }

  Json& v(const std::string& s) { pre(); quote(s); comma_ = true; return *this; }
  Json& v(double d) {
    pre();
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", std::isfinite(d) ? d : 0.0);
    out_ += b;
    comma_ = true;
    return *this;
  }
  Json& v(unsigned long long u) { pre(); out_ += std::to_string(u); comma_ = true; return *this; }
  Json& vb(bool b) { pre(); out_ += b ? "true" : "false"; comma_ = true; return *this; }

  Json& kv(const char* k, const std::string& s) { return key(k).v(s); }
  Json& kv(const char* k, const char* s) { return key(k).v(std::string(s)); }
  Json& kv(const char* k, double d) { return key(k).v(d); }
  Json& kv(const char* k, unsigned long long u) { return key(k).v(u); }
  Json& kv(const char* k, uint32_t u) { return key(k).v((unsigned long long)u); }
  Json& kvb(const char* k, bool b) { return key(k).vb(b); }
  std::string take() { return std::move(out_); }

 private:
  void pre() {
    if (comma_) out_ += ',';
  }
  void quote(const std::string& s) {
    out_ += '"';
    for (unsigned char c : s) {
      switch (c) {
        case '"': out_ += "\\\""; break;
        case '\\': out_ += "\\\\"; break;
        case '\n': out_ += "\\n"; break;
        case '\r': out_ += "\\r"; break;
        case '\t': out_ += "\\t"; break;
        default:
          if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            out_ += b;
          } else {
            out_ += char(c);
          }
      }
    }
    out_ += '"';
  }
  std::string out_;
  bool comma_ = false;
};

}  // namespace virgil
