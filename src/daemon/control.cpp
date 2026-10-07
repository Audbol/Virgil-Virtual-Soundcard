#include "control.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "dsv/engine.h"
#include "dsv/log.h"
#include "dsv/net.h"
#include "dsv/platform.h"
#include "ui_html.h"  // generated from src/daemon/ui/index.html

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#ifndef DSV_VERSION
#define DSV_VERSION "dev"
#endif

namespace dsv {

std::string default_config_path() {
#if defined(_WIN32)
  const char* pd = std::getenv("ProgramData");
  return std::string(pd ? pd : "C:\\ProgramData") + "\\DSV\\dsv.conf";
#elif defined(__APPLE__)
  return "/Library/Application Support/DSV/dsv.conf";
#else
  return "/etc/dsv/dsv.conf";
#endif
}

namespace {

// ---- tiny JSON writer -------------------------------------------------------
// Values are written in order; `comma_` says whether the next key/value in
// the current container needs a separator.
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

HttpResponse json_error(int status, const std::string& msg) {
  Json j;
  j.begin_obj().kv("error", msg).end_obj();
  return {status, "application/json", j.take(), ""};
}

const char* state_name(uint32_t s) {
  static const char* names[] = {"stopped", "free-run", "ptp-locked", "ptp-master"};
  return s < 4 ? names[s] : "unknown";
}

bool make_parent_dirs(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos || slash == 0) return true;
  const std::string dir = path.substr(0, slash);
  std::string cur;
  for (size_t i = 0; i < dir.size(); ++i) {
    cur += dir[i];
    if ((dir[i] == '/' || dir[i] == '\\') && cur.size() > 1) {
#if defined(_WIN32)
      _mkdir(cur.c_str());
#else
      mkdir(cur.c_str(), 0755);
#endif
    }
  }
#if defined(_WIN32)
  _mkdir(dir.c_str());
#else
  mkdir(dir.c_str(), 0755);
#endif
  return true;
}

}  // namespace

bool ControlServer::start(uint16_t port) {
  port_ = port;
  return http_.start(0x7F000001, port, [this](const HttpRequest& r) { return handle(r); });
}

HttpResponse ControlServer::handle(const HttpRequest& r) {
  // DNS-rebinding guard: only answer requests addressed to the loopback name.
  const std::string host = r.header("host");
  const std::string p = std::to_string(port_);
  if (host != "127.0.0.1:" + p && host != "localhost:" + p)
    return json_error(403, "requests must be addressed to 127.0.0.1 or localhost");

  if (r.method == "GET" || r.method == "HEAD") {
    if (r.path == "/" || r.path == "/index.html") {
      HttpResponse resp;
      resp.content_type = "text/html; charset=utf-8";
      resp.body.assign(reinterpret_cast<const char*>(kUiHtml), kUiHtmlSize);
      resp.extra_headers =
          "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
          "style-src 'unsafe-inline'; connect-src 'self'; img-src data:; "
          "frame-ancestors 'none'\r\n";
      return resp;
    }
    if (r.path == "/api/status") return status_json();
    if (r.path == "/api/sessions") return sessions_json();
    if (r.path == "/api/interfaces") return interfaces_json();
    if (r.path == "/api/config") return get_config();
    return json_error(404, "not found");
  }

  if (r.method == "POST") {
    // CSRF guard: a custom header forces a CORS preflight, which we never
    // approve, so other web pages cannot POST here. Also check Origin.
    if (r.header("x-dsv-request") != "1")
      return json_error(403, "missing X-DSV-Request header");
    const std::string origin = r.header("origin");
    if (!origin.empty() && origin != "http://" + host)
      return json_error(403, "cross-origin request refused");
    if (r.path == "/api/config") return save_config(r);
    if (r.path == "/api/restart") {
      ctx_->reload = true;
      Json j;
      j.begin_obj().kvb("ok", true).end_obj();
      return {200, "application/json", j.take(), ""};
    }
    return json_error(404, "not found");
  }
  return json_error(405, "method not allowed");
}

HttpResponse ControlServer::status_json() {
  std::lock_guard<std::mutex> l(ctx_->mutex);
  const Config& c = ctx_->cfg;
  Engine* e = ctx_->engine;
  ShmHeader* h = e ? e->header() : nullptr;

  Json j;
  j.begin_obj();
  j.kv("version", DSV_VERSION);
  j.kv("status", ctx_->status);
  j.kv("error", ctx_->error);
  j.kv("config_path", ctx_->config_path);

  j.key("device").begin_obj();
  j.kv("name", c.device_name);
  j.kv("interface", c.interface);
  j.kv("address", e ? ipv4_to_string(e->interface_address()) : std::string());
  j.kv("sample_rate", c.sample_rate);
  j.kv("tx_channels", c.tx_channels);
  j.kv("rx_channels", c.rx_channels);
  j.kv("packet_time_us", c.packet_time_us);
  j.kv("latency_us", c.rx_latency_us);
  j.kv("tx_lead_us", h ? uint32_t(uint64_t(h->tx_lead_frames) * 1000000 / c.sample_rate)
                       : c.tx_lead_us);
  j.end_obj();

  j.key("clock").begin_obj();
  j.kv("state", h ? state_name(h->state.load()) : "stopped");
  j.kv("offset_us", h ? double(h->ptp_offset_ns.load()) / 1000.0 : 0.0);
  j.kv("grandmaster", e ? e->grandmaster() : std::string());
  j.end_obj();

  j.key("counters").begin_obj();
  if (h) {
    j.kv("tx_packets", (unsigned long long)h->tx_packets.load());
    j.kv("rx_packets", (unsigned long long)h->rx_packets.load());
    j.kv("rx_lost", (unsigned long long)h->rx_lost.load());
    j.kv("rx_late", (unsigned long long)h->rx_late.load());
    j.kv("late_ticks", (unsigned long long)h->late_ticks.load());
    j.kv("clock_steps", (unsigned long long)h->clock_steps.load());
  }
  j.end_obj();

  j.key("clients").begin_arr();
  if (h) {
    for (uint32_t i = 0; i < kMaxTxClients; ++i) {
      const ClientSlot& s = h->clients[i];
      const uint32_t pid = s.pid.load();
      if (!pid) continue;
      j.begin_obj().kv("slot", i).kv("pid", pid).kv("name", std::string(s.name));
      j.kvb("active", s.active.load() != 0).end_obj();
    }
  }
  j.end_arr();

  j.key("tx_flows").begin_arr();
  for (const auto& s : c.tx) {
    j.begin_obj().kv("name", s.name).kv("address", s.address).kv("port", uint32_t(s.port));
    j.kv("first_channel", s.first_channel).kv("channels", s.channels);
    j.kv("encoding", s.encoding).end_obj();
  }
  j.end_arr();

  j.key("rx_flows").begin_arr();
  if (e) {
    for (const auto& st : e->rx_status()) {
      j.begin_obj().kv("sap_name", st.cfg.sap_name).kv("address", st.address);
      j.kv("port", uint32_t(st.port)).kv("first_channel", st.cfg.first_channel);
      j.kv("channels", st.cfg.channels).kvb("resolved", st.resolved);
      j.kvb("receiving", st.receiving).end_obj();
    }
  } else {
    for (const auto& s : c.rx) {
      j.begin_obj().kv("sap_name", s.sap_name).kv("address", s.address);
      j.kv("port", uint32_t(s.port)).kv("first_channel", s.first_channel);
      j.kv("channels", s.channels).kvb("resolved", false).kvb("receiving", false).end_obj();
    }
  }
  j.end_arr();

  // Meters: drain the engine at most every 80 ms and share the result.
  const int64_t now = mono_ns();
  if (e && now - ctx_->meters_ns > 80000000LL) {
    e->take_peaks(&ctx_->tx_peak, &ctx_->rx_peak);
    ctx_->meters_ns = now;
  }
  if (!e) ctx_->tx_peak.clear(), ctx_->rx_peak.clear();
  j.key("meters").begin_obj();
  j.key("tx").begin_arr();
  for (float v : ctx_->tx_peak) j.v(double(v));
  j.end_arr();
  j.key("rx").begin_arr();
  for (float v : ctx_->rx_peak) j.v(double(v));
  j.end_arr();
  j.end_obj();

  j.end_obj();
  return {200, "application/json", j.take(), ""};
}

HttpResponse ControlServer::sessions_json() {
  std::vector<SdpInfo> sessions;
  {
    std::lock_guard<std::mutex> l(ctx_->mutex);
    if (ctx_->engine) sessions = ctx_->engine->discovered();
  }
  Json j;
  j.begin_arr();
  for (const auto& s : sessions) {
    j.begin_obj().kv("name", s.session_name).kv("info", s.session_info);
    j.kv("address", s.connection_address).kv("port", uint32_t(s.port));
    j.kv("origin", s.origin_address).kv("encoding", s.encoding);
    j.kv("sample_rate", s.sample_rate).kv("channels", s.channels);
    j.kv("ptime_us", s.ptime_us).kv("grandmaster", s.ptp_grandmaster).end_obj();
  }
  j.end_arr();
  return {200, "application/json", j.take(), ""};
}

HttpResponse ControlServer::interfaces_json() {
  Json j;
  j.begin_arr();
  for (const auto& i : list_interfaces()) {
    j.begin_obj().kv("name", i.name).kv("address", ipv4_to_string(i.addr));
    j.kvb("loopback", i.loopback).end_obj();
  }
  j.end_arr();
  return {200, "application/json", j.take(), ""};
}

HttpResponse ControlServer::get_config() {
  std::string path, text;
  {
    std::lock_guard<std::mutex> l(ctx_->mutex);
    path = ctx_->config_path;
    text = format_config(ctx_->cfg);
  }
  if (!path.empty()) {
    std::ifstream f(path);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      text = ss.str();
    }
  }
  return {200, "text/plain; charset=utf-8", text, ""};
}

HttpResponse ControlServer::save_config(const HttpRequest& r) {
  Config parsed;
  std::string err;
  if (!parse_config(r.body, &parsed, &err)) return json_error(400, err);
  Config check = parsed;
  if (!validate_config(&check, &err)) return json_error(400, err);
  uint32_t addr;
  if (!check.interface.empty() && !resolve_interface(check.interface, &addr))
    return json_error(400, "network interface '" + check.interface + "' not found");

  std::string path;
  {
    std::lock_guard<std::mutex> l(ctx_->mutex);
    if (ctx_->config_path.empty()) ctx_->config_path = default_config_path();
    path = ctx_->config_path;
  }
  make_parent_dirs(path);
  const std::string tmp = path + ".new";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f || !(f << r.body) || !f.flush())
      return json_error(500, "cannot write " + path +
                                 " (permission denied? the service needs write access)");
  }
  std::remove(path.c_str());  // Windows rename does not replace
  if (std::rename(tmp.c_str(), path.c_str()) != 0)
    return json_error(500, "cannot replace " + path);
  DSV_LOG_INFO("control: configuration saved to %s; restarting engine", path.c_str());
  ctx_->reload = true;
  Json j;
  j.begin_obj().kvb("ok", true).kv("path", path).end_obj();
  return {200, "application/json", j.take(), ""};
}

}  // namespace dsv
