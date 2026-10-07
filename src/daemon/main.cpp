// dsvd - DSV virtual soundcard daemon.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "dsv/client.h"
#include "dsv/engine.h"
#include "dsv/log.h"
#include "dsv/net.h"
#include "dsv/platform.h"
#include "dsv/sap.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit = true; }

// Where installers put the configuration.
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

bool file_exists(const std::string& p) {
  if (FILE* f = std::fopen(p.c_str(), "r")) {
    std::fclose(f);
    return true;
  }
  return false;
}

void usage() {
  std::printf(
      "usage: dsvd [options]\n"
      "  -c, --config FILE     configuration file (default: the installed dsv.conf if\n"
      "                        present, else a built-in 8x8 flow on 239.69.83.67)\n"
      "  -i, --interface IF    network interface name or IPv4 address\n"
      "  -n, --name NAME       device name\n"
      "      --free-run        do not use PTP; run on the local clock\n"
      "      --packet-time US  AES67 packet time in microseconds (125/250/333/1000)\n"
      "      --latency US      receive latency in microseconds\n"
      "      --tx-lead US      playback safety margin in microseconds\n"
      "      --discover        list SAP-announced AES67/Dante streams and exit\n"
      "      --status          print statistics of a running daemon and exit\n"
      "      --log FILE        append log output to FILE\n"
#if defined(_WIN32)
      "      --service         run under the Windows service control manager\n"
#endif
      "  -v, --verbose         debug logging\n"
      "  -q, --quiet           warnings and errors only\n");
}

int discover(const std::string& iface_name) {
  uint32_t iface;
  if (!dsv::resolve_interface(iface_name, &iface)) {
    std::fprintf(stderr, "cannot find interface '%s'\n", iface_name.c_str());
    return 1;
  }
  dsv::SapService sap;
  if (!sap.start(iface)) return 1;
  std::printf("listening for SAP announcements on %s for 35 s...\n",
              dsv::ipv4_to_string(iface).c_str());
  const int64_t end = dsv::mono_ns() + 35000000000LL;
  while (!g_quit && dsv::mono_ns() < end) dsv::sleep_until_ns(dsv::mono_ns() + 200000000LL);
  for (const auto& s : sap.sessions()) {
    std::printf("\"%s\"\n    %s:%u  %s/%u/%u  ptime %.3f ms  mediaclk offset %u  from %s\n",
                s.session_name.c_str(), s.connection_address.c_str(), s.port,
                s.encoding.c_str(), s.sample_rate, s.channels, s.ptime_us / 1000.0, s.ts_offset,
                s.origin_address.c_str());
  }
  return 0;
}

int status() {
  dsv::Client c;
  if (!c.open()) {
    std::fprintf(stderr, "dsvd is not running\n");
    return 1;
  }
  const dsv::ShmHeader* h = c.header();
  static const char* states[] = {"stopped", "free-run", "ptp-locked", "ptp-master"};
  const uint32_t st = h->state.load();
  std::printf("device       %s\n", h->device_name);
  std::printf("alive        %s\n", c.daemon_alive() ? "yes" : "NO");
  std::printf("clock        %s (offset %+.1f us)\n", st < 4 ? states[st] : "?",
              h->ptp_offset_ns.load() / 1000.0);
  std::printf("format       %u Hz, %u playback / %u capture channels\n", h->sample_rate,
              h->tx_channels, h->rx_channels);
  std::printf("timing       packet %u frames, rx latency %u frames, tx lead %u frames\n",
              h->period_frames, h->rx_latency_frames, h->tx_lead_frames);
  std::printf("packets      tx %llu  rx %llu  lost %llu  late %llu\n",
              (unsigned long long)h->tx_packets.load(), (unsigned long long)h->rx_packets.load(),
              (unsigned long long)h->rx_lost.load(), (unsigned long long)h->rx_late.load());
  std::printf("scheduling   late ticks %llu, clock steps %llu\n",
              (unsigned long long)h->late_ticks.load(), (unsigned long long)h->clock_steps.load());
  for (uint32_t i = 0; i < dsv::kMaxTxClients; ++i) {
    const auto& s = h->clients[i];
    if (s.pid.load())
      std::printf("client %u     pid %u %s%s\n", i, s.pid.load(), s.name,
                  s.active.load() ? " (playing)" : "");
  }
  return 0;
}

// Runs until g_quit. `on_started` is called once the engine is up.
int run_daemon(dsv::Config cfg, void (*on_started)()) {
  // Started at boot we may be up before the NIC has an address (DHCP, link
  // negotiation). Wait for it rather than failing.
  if (on_started) on_started();
  uint32_t addr;
  bool warned = false;
  while (!g_quit && !dsv::resolve_interface(cfg.interface, &addr)) {
    if (!warned)
      DSV_LOG_WARN("waiting for network interface '%s' to come up...",
                   cfg.interface.empty() ? "(any)" : cfg.interface.c_str());
    warned = true;
    dsv::sleep_until_ns(dsv::mono_ns() + 2000000000LL);
  }
  if (g_quit) return 0;
  if (warned) DSV_LOG_INFO("network interface %s is up", dsv::ipv4_to_string(addr).c_str());

  dsv::Engine engine(cfg);
  if (!engine.start()) return 1;
  while (!g_quit) dsv::sleep_until_ns(dsv::mono_ns() + 100000000LL);
  DSV_LOG_INFO("shutting down");
  engine.stop();
  return 0;
}

dsv::Config g_cfg;

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
    DSV_LOG_INFO("service: %s requested", ctrl == SERVICE_CONTROL_STOP ? "stop" : "shutdown stop");
    report_service(SERVICE_STOP_PENDING);
    g_quit = true;
  }
}

void WINAPI service_main(DWORD, LPSTR*) {
  g_svc = RegisterServiceCtrlHandlerA("DSV", service_ctrl);
  if (!g_svc) return;
  report_service(SERVICE_START_PENDING);
  DSV_LOG_INFO("service: starting (pid %lu)", GetCurrentProcessId());
  const int rc = run_daemon(g_cfg, [] { report_service(SERVICE_RUNNING); });
  DSV_LOG_INFO("service: exiting (status %d)", rc);
  report_service(SERVICE_STOPPED, rc ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  dsv::Config& cfg = g_cfg;
  std::string config_path, iface_override, name_override, log_path;
  bool free_run = false, do_discover = false, do_status = false, loaded = false;
  bool service = false;
  uint32_t ptime = 0, latency = 0, tx_lead = 0;

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
    else if (a == "--packet-time") ptime = uint32_t(std::strtoul(next().c_str(), nullptr, 10));
    else if (a == "--latency") latency = uint32_t(std::strtoul(next().c_str(), nullptr, 10));
    else if (a == "--tx-lead") tx_lead = uint32_t(std::strtoul(next().c_str(), nullptr, 10));
    else if (a == "--discover") do_discover = true;
    else if (a == "--log") log_path = next();
    else if (a == "--service") service = true;
    else if (a == "--status") do_status = true;
    else if (a == "-v" || a == "--verbose") dsv::g_log_level = dsv::kLogDebug;
    else if (a == "-q" || a == "--quiet") dsv::g_log_level = dsv::kLogWarn;
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

  if (config_path.empty() && file_exists(default_config_path())) config_path = default_config_path();
  if (!config_path.empty()) {
    DSV_LOG_INFO("using configuration %s", config_path.c_str());
    std::string err;
    if (!dsv::load_config(config_path, &cfg, &err)) {
      std::fprintf(stderr, "%s: %s\n", config_path.c_str(), err.c_str());
      return 2;
    }
    loaded = true;
  }
  if (!iface_override.empty()) cfg.interface = iface_override;
  if (do_discover) return discover(cfg.interface);
  if (!name_override.empty()) cfg.device_name = name_override;
  if (free_run) cfg.clock = "free";
  if (ptime) cfg.packet_time_us = ptime;
  if (latency) cfg.rx_latency_us = latency;
  if (tx_lead) cfg.tx_lead_us = tx_lead;

  if (!loaded || (cfg.tx.empty() && cfg.rx.empty())) {
    // Default: one 8-channel AES67 flow each way, Dante-compatible format.
    dsv::StreamConfig tx;
    tx.address = "239.69.83.67";
    tx.channels = cfg.tx_channels < 8 ? cfg.tx_channels : 8;
    if (tx.channels) cfg.tx.push_back(tx);
  }

  if (service) {
#if defined(_WIN32)
    SERVICE_TABLE_ENTRYA table[] = {{const_cast<char*>("DSV"), service_main}, {nullptr, nullptr}};
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
  return run_daemon(cfg, nullptr);
}
