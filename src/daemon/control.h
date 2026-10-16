// Local web control panel: status, meters, discovery and configuration.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "virgil/config.h"
#include "diagnostics.h"
#include "http_server.h"
#include "remote_auth.h"

namespace virgil {

class Engine;

// State shared between the daemon's main loop and the control panel.
struct DaemonContext {
  std::mutex mutex;           // guards everything below except `reload`
  Engine* engine = nullptr;   // null while (re)starting or on error
  Config cfg;                 // configuration the engine runs / will run
  std::string config_path;    // where the panel saves configuration
  std::string status = "starting";  // starting | waiting-for-network | running | restarting | stopping | stopped | error
  std::string error;          // last start error, shown in the panel
  std::atomic<bool> reload{false};
  // Command-line options that override the file; re-applied after a reload.
  std::function<void(Config&)> overrides;
  // Stop / Start from the control panel when the file cannot keep it
  // (no configuration file): -1 = as configured, 0 = stopped, 1 = running.
  int run_override = -1;

  // Dropout diagnostics (has its own lock).
  Diagnostics diag;

  // Meter cache so several open panels see the same peaks.
  int64_t meters_ns = 0;
  std::vector<float> tx_peak, rx_peak;
};

class ControlServer {
 public:
  explicit ControlServer(DaemonContext* ctx) : ctx_(ctx) {}
  // Listens on 127.0.0.1, or on every IPv4 address when `remote` (remote
  // access on; other computers must log in with remote_password).
  bool start(uint16_t port, bool remote);
  // After a reload: listen again if remote access was turned on or off.
  void set_remote(bool remote);
  void stop() { http_.stop(); }

 private:
  HttpResponse handle(const HttpRequest& r);
  HttpResponse status_json(bool local);
  HttpResponse interfaces_json();
  HttpResponse get_config(bool local);
  std::string current_config_text();
  HttpResponse save_config(const HttpRequest& r, bool local);
  HttpResponse diagnostics_report();
  HttpResponse set_running(bool on);

  DaemonContext* ctx_;
  HttpServer http_;
  uint16_t port_ = 0;
  std::atomic<bool> remote_{false};  // listening on every IPv4 address (read by request threads)
  RemoteAuth auth_;
  // Name of the adapter Virgil runs on, for the "no clock leader" notice
  // (looked up once per address; guarded by ctx_->mutex).
  uint32_t adapter_addr_ = 0;
  std::string adapter_name_;
};

// Wall clock in ms since the Unix epoch (diagnostics timestamps).
int64_t wall_ms();

// Version tag of a configuration text (X-Virgil-Config-Version).
std::string config_version(const std::string& text);

// The configuration file location used by installers on this OS.
std::string default_config_path();

}  // namespace virgil
