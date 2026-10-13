#include "control.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "virgil/engine.h"
#include "virgil/log.h"
#include "virgil/net.h"
#include "virgil/platform.h"
#include "virgil/ptp.h"
#include "json.h"
#include "ui_html.h"  // generated from src/daemon/ui/index.html

#if defined(_WIN32)
#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef interface  // objbase.h macro; clashes with Config::interface
#else
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifndef VIRGIL_VERSION
#define VIRGIL_VERSION "dev"
#endif

namespace virgil {

int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string default_config_path() {
#if defined(_WIN32)
  const char* pd = std::getenv("ProgramData");
  return std::string(pd ? pd : "C:\\ProgramData") + "\\Virgil\\virgil.conf";
#elif defined(__APPLE__)
  return "/Library/Application Support/Virgil/virgil.conf";
#else
  return "/etc/virgil/virgil.conf";
#endif
}

namespace {

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

bool ControlServer::start(uint16_t port, bool remote) {
  port_ = port;
  auto handler = [this](const HttpRequest& r) { return handle(r); };
  if (remote) {
    if (http_.start(0, port, handler)) {
      remote_ = true;
      std::string urls;
      for (const auto& i : list_interfaces())
        if (!i.loopback) urls += " http://" + ipv4_to_string(i.addr) + ":" + std::to_string(port) + "/";
      VIRGIL_LOG_INFO("control panel: remote access on (password required):%s", urls.c_str());
      return true;
    }
    VIRGIL_LOG_WARN("control panel: cannot listen on the network on port %u; this computer only", port);
  }
  remote_ = false;
  return http_.start(0x7F000001, port, handler);
}

void ControlServer::set_remote(bool remote) {
  if (remote == remote_ || !port_) return;
  http_.stop();
  if (!start(port_, remote)) VIRGIL_LOG_WARN("control panel: port %u is in use; panel disabled", port_);
  else if (!remote) VIRGIL_LOG_INFO("control panel: remote access off; http://127.0.0.1:%u/ only", port_);
}

HttpResponse ControlServer::handle(const HttpRequest& r) {
  const std::string host = r.header("host");
  const bool local = is_loopback(r.peer);
  if (local) {
    // DNS-rebinding guard: only answer requests addressed to the loopback name.
    const std::string p = std::to_string(port_);
    if (host != "127.0.0.1:" + p && host != "localhost:" + p)
      return json_error(403, "requests must be addressed to 127.0.0.1 or localhost");
  } else {
    // Another computer (remote access): it must log in with remote_password.
    bool enabled;
    std::string password;
    {
      std::lock_guard<std::mutex> l(ctx_->mutex);
      enabled = ctx_->cfg.remote_access;
      password = ctx_->cfg.remote_password;
    }
    const int64_t now = mono_ns() / 1000000;
    switch (auth_.check(r.peer, enabled, password, r.header("authorization"), now)) {
      case RemoteAuth::Result::Allowed: break;
      case RemoteAuth::Result::NeedLogin: {
        HttpResponse resp = json_error(401, "log in with the remote access password");
        resp.extra_headers = "WWW-Authenticate: Basic realm=\"Virgil\", charset=\"UTF-8\"\r\n";
        return resp;
      }
      case RemoteAuth::Result::LockedOut: {
        HttpResponse resp = json_error(429, "too many wrong passwords; try again shortly");
        resp.extra_headers = "Retry-After: " + std::to_string(auth_.retry_after_s(r.peer, now)) + "\r\n";
        return resp;
      }
      case RemoteAuth::Result::Refused: return json_error(403, "remote access is off");
    }
  }

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
    if (r.path == "/api/status") return status_json(local);
    if (r.path == "/api/interfaces") return interfaces_json();
    if (r.path == "/api/config") return get_config(local);
    if (r.path == "/api/diagnostics")
      return {200, "application/json", ctx_->diag.json(wall_ms()), ""};
    if (r.path == "/api/diagnostics/report") return diagnostics_report();
    return json_error(404, "not found");
  }

  if (r.method == "POST") {
    // CSRF guard: a custom header forces a CORS preflight, which we never
    // approve, so other web pages cannot POST here. Also check Origin.
    if (r.header("x-virgil-request") != "1")
      return json_error(403, "missing X-Virgil-Request header");
    const std::string origin = r.header("origin");
    if (!origin.empty() && origin != "http://" + host)
      return json_error(403, "cross-origin request refused");
    if (r.path == "/api/config") return save_config(r, local);
    if (r.path == "/api/diagnostics/reset") {
      ctx_->diag.reset(wall_ms());
      Json j;
      j.begin_obj().kvb("ok", true).end_obj();
      return {200, "application/json", j.take(), ""};
    }
    if (r.path == "/api/stop") return set_running(false);
    if (r.path == "/api/start") return set_running(true);
    if (r.path == "/api/restart") {
      {
        std::lock_guard<std::mutex> l(ctx_->mutex);
        if (ctx_->status != "stopped") ctx_->status = "restarting";
      }
      ctx_->reload = true;
      Json j;
      j.begin_obj().kvb("ok", true).end_obj();
      return {200, "application/json", j.take(), ""};
    }
    return json_error(404, "not found");
  }
  return json_error(405, "method not allowed");
}

HttpResponse ControlServer::status_json(bool local) {
  std::lock_guard<std::mutex> l(ctx_->mutex);
  const Config& c = ctx_->cfg;
  Engine* e = ctx_->engine;
  ShmHeader* h = e ? e->header() : nullptr;

  Json j;
  j.begin_obj();
  j.kv("version", VIRGIL_VERSION);
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
  j.kv("latency_us", c.latency_us);
  j.kv("tx_latency_us", c.tx_latency_us);
  j.kvb("dante", e ? e->dante_running() : false);
  j.kvb("enabled", c.enabled);
  j.kv("dante_ports", e ? uint32_t(e->control_port_base()) : 0u);
  j.end_obj();

  // Channel names as on the network (Dante Controller, the drivers).
  for (int dir = 0; dir < 2; ++dir) {
    const bool tx = dir == 0;
    const uint32_t n = tx ? c.tx_channels : c.rx_channels;
    std::vector<std::string> names = e ? e->channel_names(tx) : std::vector<std::string>();
    j.key(tx ? "tx_names" : "rx_names").begin_arr();
    for (uint32_t i = 0; i < n; ++i) {
      std::string v = i < names.size() ? names[i] : (tx ? c.tx_name(i) : c.rx_name(i));
      if (v.empty()) v = default_channel_name(tx, i + 1);
      j.v(v);
    }
    j.end_arr();
  }
  j.kv("dropouts_last_hour", (unsigned long long)ctx_->diag.recent_dropouts(wall_ms()));

  // Remote access to this panel from other computers.
  j.key("remote").begin_obj();
  j.kvb("enabled", c.remote_access);
  j.kvb("listening", remote_);
  j.kvb("this_client_remote", !local);
  j.key("urls").begin_arr();
  if (remote_)
    for (const auto& i : list_interfaces())
      if (!i.loopback) j.v("http://" + ipv4_to_string(i.addr) + ":" + std::to_string(port_) + "/");
  j.end_arr();
  j.end_obj();

  j.key("clock").begin_obj();
  j.kv("state", h ? state_name(h->state.load()) : "stopped");
  j.kv("offset_us", h ? double(h->ptp_offset_ns.load()) / 1000.0 : 0.0);
  j.kv("grandmaster", e ? e->grandmaster() : std::string());
  j.kv("source", c.clock);
  // What the PTP follower has heard (clock = ptp only, engine running): the
  // apps warn when no Dante clock leader is found. Leaves out the leader
  // case, where nobody else has to be heard.
  const PtpClock* ptp = e ? dynamic_cast<const PtpClock*>(e->clock_source()) : nullptr;
  if (ptp && h && h->state.load() != kStatePtpMaster) {
    j.key("ptp").begin_obj();
    j.kv("senders", uint32_t(ptp->senders_seen()));
    j.kvb("master_heard", ptp->master_ever_heard());
    const double quiet = ptp->seconds_without_master();
    j.kv("no_leader_s", std::floor(quiet * 10.0) / 10.0);
    // Adapter name for "No Dante clock leader found on <adapter> (<ip>)";
    // only looked up while there is something to say.
    std::string adapter;
    if (quiet > 0) {
      const uint32_t addr = e->interface_address();
      if (adapter_addr_ != addr || adapter_name_.empty()) {
        adapter_addr_ = addr;
        adapter_name_.clear();
        for (const auto& i : list_interfaces())
          if (i.addr == addr) {
            adapter_name_ = i.name;
            break;
          }
      }
      adapter = adapter_name_;
    }
    j.kv("adapter", adapter);
    j.end_obj();
  }
  j.end_obj();

  j.key("counters").begin_obj();
  if (h) {
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
      j.begin_obj().kv("slot", i).kv("pid", pid).kv("name", slot_name(s));
      j.kvb("active", s.active.load() != 0).end_obj();
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

HttpResponse ControlServer::interfaces_json() {
  Json j;
  j.begin_arr();
  for (const auto& i : list_interfaces()) {
    j.begin_obj().kv("name", i.name).kv("address", ipv4_to_string(i.addr));
    j.kvb("loopback", i.loopback).kvb("virtual", i.virtual_adapter).end_obj();
  }
  j.end_arr();
  return {200, "application/json", j.take(), ""};
}

// Plain-text report to attach to a bug report or send to whoever looks
// after the system: versions, setup, dropout events and the recent log.
HttpResponse ControlServer::diagnostics_report() {
  const int64_t now = wall_ms();
  std::string r = "Virgil diagnostics report\n=========================\n";
  r += std::string("Virgil ") + VIRGIL_VERSION + " on ";
#if defined(_WIN32)
  r += "Windows";
#elif defined(__APPLE__)
  r += "macOS";
#else
  r += "Linux";
#endif
#if !defined(_WIN32)
  struct utsname u;
  if (uname(&u) == 0) r += std::string(" (") + u.sysname + " " + u.release + " " + u.machine + ")";
#endif
  r += ", computer " + host_name() + "\n";
  std::string cfg_path;
  {
    std::lock_guard<std::mutex> l(ctx_->mutex);
    const Config& c = ctx_->cfg;
    Engine* e = ctx_->engine;
    ShmHeader* h = e ? e->header() : nullptr;
    cfg_path = ctx_->config_path;
    char b[512];
    std::snprintf(b, sizeof b,
                  "\nDevice  \"%s\" on %s, %u Hz, %u out / %u in, receive latency %.1f ms, transmit latency "
                  "%.1f ms\nService %s%s%s\n",
                  c.device_name.c_str(), e ? ipv4_to_string(e->interface_address()).c_str() : "-", c.sample_rate,
                  c.tx_channels, c.rx_channels, c.latency_us / 1000.0, c.tx_latency_us / 1000.0,
                  ctx_->status.c_str(), ctx_->error.empty() ? "" : ": ", ctx_->error.c_str());
    r += b;
    if (h) {
      std::snprintf(b, sizeof b, "Clock   %s, leader %s, offset %+.1f us\n", state_name(h->state.load()),
                    e->grandmaster().empty() ? "-" : e->grandmaster().c_str(), h->ptp_offset_ns.load() / 1000.0);
      r += b;
      for (uint32_t i = 0; i < kMaxTxClients; ++i) {
        const ClientSlot& s = h->clients[i];
        if (!s.pid.load()) continue;
        std::snprintf(b, sizeof b, "App     %s (process %u)%s\n", slot_name(s).c_str(), s.pid.load(),
                      s.active.load() ? ", playing" : "");
        r += b;
      }
    }
  }
  r += "\nNetwork adapters\n";
  for (const auto& i : list_interfaces())
    r += "  " + i.name + "  " + ipv4_to_string(i.addr) + (i.loopback ? "  loopback" : "") +
         (i.virtual_adapter ? "  virtual" : "") + "\n";
  r += "\n" + ctx_->diag.report(now);
  if (!cfg_path.empty()) {
    std::ifstream f(cfg_path);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      r += "\nConfiguration (" + cfg_path + ")\n" + ss.str();
      if (!r.empty() && r.back() != '\n') r += "\n";
    }
  }
  r += "\nRecent log\n";
  for (const auto& line : recent_log_lines()) r += "  " + line + "\n";
  HttpResponse resp{200, "text/plain; charset=utf-8", r, ""};
  resp.extra_headers = "Content-Disposition: attachment; filename=\"virgil-report.txt\"\r\n";
  return resp;
}

// Stop / Start Virgil: kept in the configuration ([device] enabled), so a
// stopped Virgil stays stopped after a restart of the computer, and applied
// by a reload of the engine.
HttpResponse ControlServer::set_running(bool on) {
  std::string path;
  {
    std::lock_guard<std::mutex> l(ctx_->mutex);
    ctx_->run_override = on ? 1 : 0;
    path = ctx_->config_path;
    // Shown until the engine has stopped / started (no "Dante stack not
    // running" while it reloads).
    if (on) {
      if (ctx_->status == "stopped" || ctx_->status == "stopping") ctx_->status = "starting";
    } else {
      ctx_->status = ctx_->status == "stopped" ? "stopped" : "stopping";
    }
  }
  bool saved = false;
  std::string err;
  if (!path.empty()) {
    std::ifstream exists(path);
    if (exists) {
      exists.close();
      saved = set_config_value(path, "device", "enabled", on ? "" : "false", &err);
      if (!saved) VIRGIL_LOG_WARN("could not save %s state: %s", on ? "started" : "stopped", err.c_str());
    }
  }
  {
    // Kept by the file: later edits of `enabled` there (Settings, by hand)
    // must win again. Only an unsaved state stays in memory.
    std::lock_guard<std::mutex> l(ctx_->mutex);
    if (saved) ctx_->run_override = -1;
  }
  ctx_->diag.note(Diagnostics::Kind::Service, on ? "Virgil was started" : "Virgil was stopped", wall_ms());
  VIRGIL_LOG_INFO("control: %s", on ? "starting Virgil" : "stopping Virgil");
  ctx_->reload = true;
  Json j;
  j.begin_obj().kvb("ok", true).kvb("saved", saved).end_obj();
  return {200, "application/json", j.take(), ""};
}

// The configuration as the panel edits it: the file, or (no file yet) the
// running configuration. Hold config_file_mutex() for a consistent read.
std::string ControlServer::current_config_text() {
  std::string path, text;
  {
    std::lock_guard<std::mutex> l(ctx_->mutex);
    path = ctx_->config_path;
    text = format_config(ctx_->cfg);
  }
  if (!path.empty()) {
    std::ifstream f(path, std::ios::binary);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      text = ss.str();
    }
  }
  return text;
}

// Version of a configuration text: GET /api/config returns it (header
// X-Virgil-Config-Version), a POST may send it back so a save based on an
// older text (changed meanwhile by Stop, a rename in Dante Controller or
// another editor) is refused with 409 instead of undoing those changes.
std::string config_version(const std::string& text) {
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (unsigned char ch : text) h = (h ^ ch) * 1099511628211ull;
  char b[20];
  std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
  return b;
}

HttpResponse ControlServer::get_config(bool local) {
  std::string text;
  {
    std::lock_guard<std::mutex> lock(config_file_mutex());
    text = current_config_text();
  }
  // Other computers never see the password; the version is of the real text.
  HttpResponse resp{200, "text/plain; charset=utf-8", local ? text : mask_remote_password(text), ""};
  resp.extra_headers = "X-Virgil-Config-Version: " + config_version(text) + "\r\n";
  return resp;
}

HttpResponse ControlServer::save_config(const HttpRequest& r, bool local) {
  // A password sent back masked keeps the current one; other computers may
  // not change the remote access settings.
  std::string body, err;
  {
    std::lock_guard<std::mutex> lock(config_file_mutex());
    if (!resolve_posted_config(r.body, current_config_text(), local, &body, &err)) return json_error(403, err);
  }
  Config parsed;
  if (!parse_config(body, &parsed, &err)) return json_error(400, err);
  Config check = parsed;
  if (!validate_config(&check, &err)) return json_error(400, err);

  std::string path, running_iface;
  {
    std::lock_guard<std::mutex> l(ctx_->mutex);
    if (ctx_->config_path.empty()) ctx_->config_path = default_config_path();
    path = ctx_->config_path;
    running_iface = ctx_->cfg.interface;
  }
  make_parent_dirs(path);
  const std::string base = r.header("x-virgil-config-version");
  {
    std::lock_guard<std::mutex> lock(config_file_mutex());
    const std::string current = current_config_text();
    if (!base.empty() && base != config_version(current))
      return json_error(409, "the configuration was changed elsewhere meanwhile; reload it and try again");
    // A missing network adapter is only refused when this save chooses it:
    // the daemon waits for a configured one that is not there (yet), so
    // other settings can still be saved meanwhile.
    Config before;
    std::string ignored;
    const std::string old_iface = parse_config(current, &before, &ignored) ? before.interface : running_iface;
    uint32_t addr;
    if (!check.interface.empty() && check.interface != old_iface && !resolve_interface(check.interface, &addr))
      return json_error(400, "network interface '" + check.interface + "' not found");
    std::string werr;
    if (!write_config_file(path, body, &werr))
      return json_error(500, werr + " (permission denied? the service needs write access to " + path + ")");
  }
  const bool enabled = check.enabled;
  {
    std::lock_guard<std::mutex> l(ctx_->mutex);
    ctx_->run_override = -1;  // the saved file says whether Virgil runs now
    if (enabled) ctx_->status = "restarting";
    else if (ctx_->status != "stopped") ctx_->status = "stopping";
  }
  VIRGIL_LOG_INFO("control: configuration saved to %s; restarting engine", path.c_str());
  ctx_->reload = true;
  Json j;
  j.begin_obj().kvb("ok", true).kv("path", path).kvb("enabled", enabled);
  j.kv("version", config_version(body)).end_obj();
  return {200, "application/json", j.take(), ""};
}

}  // namespace virgil
