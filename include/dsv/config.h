// Daemon configuration (INI-style file, see config/virgil.conf.example).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsv {

struct Config {
  std::string device_name = "Virgil";  // name in Dante Controller (<= 31 chars)
  std::string interface;  // name or IPv4 of the Dante network NIC; empty = first active
  std::string shm_name;
  uint32_t sample_rate = 48000;
  uint32_t tx_channels = 8;        // Dante transmit channels (computer -> network)
  uint32_t rx_channels = 8;        // Dante receive channels (network -> computer)
  uint32_t latency_us = 4000;      // Dante receive latency
  uint32_t tx_latency_us = 4000;   // Dante transmit latency
  uint32_t tick_us = 1000;         // internal mixing period
  uint32_t tx_lead_us = 0;         // drivers' playback margin; 0 = two ticks
  uint32_t ring_frames = 0;        // 0 = auto
  int rt_priority = 80;
  uint32_t spin_us = 50;
  bool lock_memory = true;

  std::string clock = "ptp";          // ptp | free
  std::string ptp_subdomain = "_DFLT";
  bool ptp_master_capable = false;    // only for networks without Dante hardware

  uint32_t control_port = 8480;       // local web control panel; 0 = off

  // Messages about ignored legacy settings, for the log.
  std::vector<std::string> notes;

  uint32_t us_to_frames(uint32_t us) const {
    return uint32_t((uint64_t(sample_rate) * us + 500000) / 1000000);
  }
  uint32_t period_frames() const { return us_to_frames(tick_us); }
  // How far capture reads trail the clock (drivers): Inferno already applies
  // the Dante receive latency; this only covers the hand-over ticks.
  uint32_t rx_latency_frames() const { return 3 * period_frames(); }
  uint32_t tx_lead_frames() const {
    return tx_lead_us ? us_to_frames(tx_lead_us) : 2 * period_frames();
  }
};

// Parse INI text. On failure returns false and fills `error` ("line N: ...").
bool parse_config(const std::string& text, Config* out, std::string* error);
bool load_config(const std::string& path, Config* out, std::string* error);
// Range checks and derived defaults. Returns false with a message if unusable.
bool validate_config(Config* c, std::string* error);
// Serialise back to INI text that parse_config() reads.
std::string format_config(const Config& c);

}  // namespace dsv
