// Local web control panel: status, meters, discovery and configuration.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "virgil/config.h"
#include "http_server.h"

namespace virgil {

class Engine;

// State shared between the daemon's main loop and the control panel.
struct DaemonContext {
  std::mutex mutex;           // guards everything below except `reload`
  Engine* engine = nullptr;   // null while (re)starting or on error
  Config cfg;                 // configuration the engine runs / will run
  std::string config_path;    // where the panel saves configuration
  std::string status = "starting";  // starting | waiting-for-network | running | error
  std::string error;          // last start error, shown in the panel
  std::atomic<bool> reload{false};

  // Meter cache so several open panels see the same peaks.
  int64_t meters_ns = 0;
  std::vector<float> tx_peak, rx_peak;
};

class ControlServer {
 public:
  explicit ControlServer(DaemonContext* ctx) : ctx_(ctx) {}
  bool start(uint16_t port);
  void stop() { http_.stop(); }

 private:
  HttpResponse handle(const HttpRequest& r);
  HttpResponse status_json();
  HttpResponse interfaces_json();
  HttpResponse get_config();
  HttpResponse save_config(const HttpRequest& r);

  DaemonContext* ctx_;
  HttpServer http_;
  uint16_t port_ = 0;
};

// The configuration file location used by installers on this OS.
std::string default_config_path();

}  // namespace virgil
