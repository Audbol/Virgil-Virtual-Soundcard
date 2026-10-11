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
    if (r.path == "/api/interfaces") return interfaces_json();
    if (r.path == "/api/config") return get_config();
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
    if (r.path == "/api/config") return save_config(r);
    if (r.path == "/api/diagnostics/reset") {
      ctx_->diag.reset(wall_ms());
      Json j;
      j.begin_obj().kvb("ok", true).end_obj();
      return {200, "application/json", j.take(), ""};
    }
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

  j.key("clock").begin_obj();
  j.kv("state", h ? state_name(h->state.load()) : "stopped");
  j.kv("offset_us", h ? double(h->ptp_offset_ns.load()) / 1000.0 : 0.0);
  j.kv("grandmaster", e ? e->grandmaster() : std::string());
  j.kv("source", c.clock);
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
    // Written to disk before it replaces the old file, which is replaced in
    // one step: a crash or power cut leaves either the old or the new file.
    FILE* f = std::fopen(tmp.c_str(), "wb");
    bool ok = f && std::fwrite(r.body.data(), 1, r.body.size(), f) == r.body.size() && std::fflush(f) == 0;
#if !defined(_WIN32)
    if (ok) ok = fsync(fileno(f)) == 0;
#endif
    if (f && std::fclose(f) != 0) ok = false;
    if (!ok) {
      std::remove(tmp.c_str());
      return json_error(500, "cannot write " + path +
                                 " (permission denied? the service needs write access)");
    }
  }
#if defined(_WIN32)
  if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return json_error(500, "cannot replace " + path);
#else
  if (std::rename(tmp.c_str(), path.c_str()) != 0) return json_error(500, "cannot replace " + path);
#endif
  VIRGIL_LOG_INFO("control: configuration saved to %s; restarting engine", path.c_str());
  ctx_->reload = true;
  Json j;
  j.begin_obj().kvb("ok", true).kv("path", path).end_obj();
  return {200, "application/json", j.take(), ""};
}

}  // namespace virgil
