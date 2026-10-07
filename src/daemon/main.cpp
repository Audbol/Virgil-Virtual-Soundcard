// virgild - Virgil virtual soundcard daemon.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "virgil/client.h"
#include "virgil/engine.h"
#include "virgil/log.h"
#include "virgil/net.h"
#include "virgil/platform.h"
#include "virgil/shm.h"
#include "virgil/shm_layout.h"
#include "control.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit = true; }

bool file_exists(const std::string& p) {
  if (FILE* f = std::fopen(p.c_str(), "r")) {
    std::fclose(f);
    return true;
  }
  return false;
}

void usage() {
  std::printf(
      "usage: virgild [options]\n"
      "  -c, --config FILE     configuration file (default: the installed virgil.conf if\n"
      "                        present, else built-in defaults: 8x8 channels named Virgil)\n"
      "  -i, --interface IF    network interface name or IPv4 address\n"
      "  -n, --name NAME       device name\n"
      "      --free-run        do not use PTP; run on the local clock\n"
      "      --latency US      Dante receive latency in microseconds\n"
      "      --tx-latency US   Dante transmit latency in microseconds\n"
      "      --control-port N  web control panel port on 127.0.0.1 (0 = off; default 8480)\n"
      "      --status          print statistics of a running daemon and exit\n"
      "      --log FILE        append log output to FILE\n"
#if defined(_WIN32)
      "      --service         run under the Windows service control manager\n"
#endif
      "  -v, --verbose         debug logging\n"
      "  -q, --quiet           warnings and errors only\n");
}

int status() {
  virgil::Client c;
  if (!c.open()) {
    std::fprintf(stderr, "virgild is not running\n");
    return 1;
  }
  const virgil::ShmHeader* h = c.header();
  static const char* states[] = {"stopped", "free-run", "ptp-locked", "ptp-master"};
  const uint32_t st = h->state.load();
  std::printf("device       %s\n", h->device_name);
  std::printf("alive        %s\n", c.daemon_alive() ? "yes" : "NO");
  std::printf("clock        %s (offset %+.1f us)\n", st < 4 ? states[st] : "?",
              h->ptp_offset_ns.load() / 1000.0);
  std::printf("format       %u Hz, %u playback / %u capture channels\n", h->sample_rate,
              h->tx_channels, h->rx_channels);
  std::printf("timing       tick %u frames, capture lag %u frames, playback lead %u frames\n",
              h->period_frames, h->rx_latency_frames, h->tx_lead_frames);
  std::printf("scheduling   late ticks %llu, clock steps %llu\n",
              (unsigned long long)h->late_ticks.load(), (unsigned long long)h->clock_steps.load());
  for (uint32_t i = 0; i < virgil::kMaxTxClients; ++i) {
    const auto& s = h->clients[i];
    if (s.pid.load())
      std::printf("client %u     pid %u %s%s\n", i, s.pid.load(), s.name,
                  s.active.load() ? " (playing)" : "");
  }
  return 0;
}

// Runs until g_quit. `on_started` is called once, right away (services must
// report RUNNING quickly). The control panel can ask for a reload: the
// engine is then stopped, the configuration re-read and the engine started
// again, while the process (and the panel) stay up.
int run_daemon(virgil::DaemonContext& ctx, void (*on_started)()) {
  if (on_started) on_started();

  virgil::ControlServer control(&ctx);
  const uint16_t control_port = uint16_t(ctx.cfg.control_port);
  if (control_port) {
    if (control.start(control_port))
      VIRGIL_LOG_INFO("control panel: http://127.0.0.1:%u/", control_port);
    else
      VIRGIL_LOG_WARN("control panel: port %u is in use; panel disabled", control_port);
  }

  auto set_status = [&](const char* st, const std::string& err = std::string()) {
    std::lock_guard<std::mutex> l(ctx.mutex);
    ctx.status = st;
    ctx.error = err;
  };
  auto wait_reload = [&](int64_t max_ns) {
    const int64_t end = virgil::mono_ns() + max_ns;
    while (!g_quit && !ctx.reload && virgil::mono_ns() < end)
      virgil::sleep_until_ns(virgil::mono_ns() + 100000000LL);
  };

  // The soundcard apps attach to also outlives engine restarts.
  virgil::SharedMemory soundcard;

  int rc = 0;
  while (!g_quit) {
    virgil::Config cfg;
    {
      std::lock_guard<std::mutex> l(ctx.mutex);
      cfg = ctx.cfg;
    }

    // At boot the NIC may not have an address yet (DHCP, link): wait for it.
    uint32_t addr = 0;
    bool warned = false;
    while (!g_quit && !ctx.reload && !virgil::resolve_interface(cfg.interface, &addr)) {
      if (!warned) {
        VIRGIL_LOG_WARN("waiting for network interface '%s' to come up...",
                     cfg.interface.empty() ? "(any)" : cfg.interface.c_str());
        set_status("waiting-for-network");
      }
      warned = true;
      wait_reload(2000000000LL);
    }
    if (warned && !g_quit && !ctx.reload)
      VIRGIL_LOG_INFO("network interface %s is up", virgil::ipv4_to_string(addr).c_str());

    std::unique_ptr<virgil::Engine> engine;
    if (!g_quit && !ctx.reload) {
      virgil::clear_last_error();
      engine = std::make_unique<virgil::Engine>(cfg);
      engine->set_shared_memory(&soundcard);
      if (engine->start()) {
        std::lock_guard<std::mutex> l(ctx.mutex);
        ctx.engine = engine.get();
        ctx.cfg = engine->config();  // with defaults filled in by validation
        ctx.status = "running";
        ctx.error.clear();
        rc = 0;
      } else {
        const std::string err = virgil::last_error_message();
        engine.reset();
        rc = 1;
        // Without the panel nobody can fix it from here: exit (the service
        // manager restarts us). With it, stay up and show the error.
        if (!control_port) break;
        set_status("error", err.empty() ? "engine failed to start" : err);
      }
    }

    while (!g_quit && !ctx.reload) wait_reload(1000000000LL);

    if (engine) {
      {
        std::lock_guard<std::mutex> l(ctx.mutex);
        ctx.engine = nullptr;
      }
      VIRGIL_LOG_INFO(g_quit ? "shutting down" : "stopping engine for reload");
      engine->stop();
      engine.reset();
    }

    if (ctx.reload.exchange(false)) {
      std::string path;
      {
        std::lock_guard<std::mutex> l(ctx.mutex);
        path = ctx.config_path;
      }
      virgil::Config fresh;
      std::string err;
      if (!path.empty() && virgil::load_config(path, &fresh, &err)) {
        std::lock_guard<std::mutex> l(ctx.mutex);
        fresh.control_port = ctx.cfg.control_port;  // the panel stays where it is
        ctx.cfg = fresh;
        VIRGIL_LOG_INFO("reloaded configuration %s", path.c_str());
      } else if (!path.empty()) {
        VIRGIL_LOG_ERROR("reload of %s failed: %s", path.c_str(), err.c_str());
      }
      set_status("starting");
    }
  }
  control.stop();
  if (soundcard.is_open())
    static_cast<virgil::ShmHeader*>(soundcard.data())->state.store(virgil::kStateStopped);
  return g_quit ? 0 : rc;
}

virgil::DaemonContext g_ctx;

#if defined(_WIN32)
SERVICE_STATUS_HANDLE g_svc = nullptr;
SERVICE_STATUS g_svc_status{};

void report_service(DWORD state, DWORD exit_code = NO_ERROR) {
  g_svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_svc_status.dwCurrentState = state;
  g_svc_status.dwWin32ExitCode = exit_code;
  g_svc_status.dwControlsAccepted =
      state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
  g_svc_status.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 10000;
  SetServiceStatus(g_svc, &g_svc_status);
}

void WINAPI service_ctrl(DWORD ctrl) {
  if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
    VIRGIL_LOG_INFO("service: %s requested", ctrl == SERVICE_CONTROL_STOP ? "stop" : "shutdown stop");
    report_service(SERVICE_STOP_PENDING);
    g_quit = true;
  }
}

void WINAPI service_main(DWORD, LPSTR*) {
  g_svc = RegisterServiceCtrlHandlerA("Virgil", service_ctrl);
  if (!g_svc) return;
  report_service(SERVICE_START_PENDING);
  VIRGIL_LOG_INFO("service: starting (pid %lu)", GetCurrentProcessId());
  const int rc = run_daemon(g_ctx, [] { report_service(SERVICE_RUNNING); });
  VIRGIL_LOG_INFO("service: exiting (status %d)", rc);
  report_service(SERVICE_STOPPED, rc ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  virgil::Config& cfg = g_ctx.cfg;
  std::string config_path, iface_override, name_override, log_path;
  bool free_run = false, do_status = false;
  bool service = false;
  uint32_t latency = 0, tx_latency = 0;
  long control_port = -1;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs an argument\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-c" || a == "--config") config_path = next();
    else if (a == "-i" || a == "--interface") iface_override = next();
    else if (a == "-n" || a == "--name") name_override = next();
    else if (a == "--free-run") free_run = true;
    else if (a == "--latency") latency = uint32_t(std::strtoul(next().c_str(), nullptr, 10));
    else if (a == "--tx-latency") tx_latency = uint32_t(std::strtoul(next().c_str(), nullptr, 10));
    else if (a == "--log") log_path = next();
    else if (a == "--control-port") control_port = std::strtol(next().c_str(), nullptr, 10);
    else if (a == "--service") service = true;
    else if (a == "--status") do_status = true;
    else if (a == "-v" || a == "--verbose") virgil::g_log_level = virgil::kLogDebug;
    else if (a == "-q" || a == "--quiet") virgil::g_log_level = virgil::kLogWarn;
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else { usage(); return 2; }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (do_status) return status();

  if (!log_path.empty()) {
    if (!std::freopen(log_path.c_str(), "a", stderr)) {
      std::fprintf(stdout, "cannot open log file %s\n", log_path.c_str());
      return 2;
    }
    std::setvbuf(stderr, nullptr, _IOLBF, 1024);
  }

  if (config_path.empty() && file_exists(virgil::default_config_path()))
    config_path = virgil::default_config_path();
  if (!config_path.empty()) {
    VIRGIL_LOG_INFO("using configuration %s", config_path.c_str());
    std::string err;
    if (!virgil::load_config(config_path, &cfg, &err)) {
      std::fprintf(stderr, "%s: %s\n", config_path.c_str(), err.c_str());
      return 2;
    }
  }
  if (!iface_override.empty()) cfg.interface = iface_override;
  if (!name_override.empty()) cfg.device_name = name_override;
  if (free_run) cfg.clock = "free";
  if (latency) cfg.latency_us = latency;
  if (tx_latency) cfg.tx_latency_us = tx_latency;
  if (control_port >= 0 && control_port <= 65535) cfg.control_port = uint32_t(control_port);
  g_ctx.config_path = config_path;

  for (const auto& n : cfg.notes) VIRGIL_LOG_WARN("config: %s", n.c_str());

  if (service) {
#if defined(_WIN32)
    SERVICE_TABLE_ENTRYA table[] = {{const_cast<char*>("Virgil"), service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherA(table)) {
      std::fprintf(stderr, "--service must be started by the service control manager\n");
      return 1;
    }
    return 0;
#else
    std::fprintf(stderr, "--service is Windows-only; use systemd or launchd\n");
    return 2;
#endif
  }
  return run_daemon(g_ctx, nullptr);
}
