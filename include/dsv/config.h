// Daemon configuration (INI-style file, see config/dsv.conf.example).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsv {

struct StreamConfig {
  std::string name;              // tx: SAP session name
  std::string address;           // multicast group or unicast IPv4
  uint16_t port = 5004;
  uint32_t first_channel = 1;    // 1-based device channel
  uint32_t channels = 8;
  uint8_t payload_type = 97;
  uint32_t ttl = 32;
  std::string encoding = "L24";  // L24 | L16
  std::string sap_name;          // rx: follow a SAP-announced session by name
  std::string source;            // rx: only accept packets from this sender
  uint32_t ts_offset = 0;        // rx: RTP offset (a=mediaclk:direct=)
};

struct Config {
  std::string device_name = "DSV Virtual Soundcard";
  std::string interface;  // name or IPv4 of the Dante/AES67 network NIC
  std::string shm_name;
  uint32_t sample_rate = 48000;
  uint32_t tx_channels = 8;
  uint32_t rx_channels = 8;
  uint32_t packet_time_us = 1000;  // 125, 250, 333, 1000 (AES67 / Dante)
  uint32_t rx_latency_us = 2000;   // receive buffer, like Dante "latency"
  uint32_t ring_frames = 0;        // 0 = auto
  int rt_priority = 80;
  uint32_t spin_us = 50;           // busy-wait before each tick
  bool lock_memory = true;

  std::string clock = "ptp";       // ptp | free
  uint32_t ptp_domain = 0;
  bool ptp_master_capable = true;
  uint32_t ptp_priority1 = 250;

  bool sap = true;
  std::vector<StreamConfig> tx;
  std::vector<StreamConfig> rx;

  uint32_t period_frames() const {
    return uint32_t((uint64_t(sample_rate) * packet_time_us + 500000) / 1000000);
  }
  uint32_t rx_latency_frames() const {
    return uint32_t((uint64_t(sample_rate) * rx_latency_us + 500000) / 1000000);
  }
};

// Parse INI text. On failure returns false and fills `error` ("line N: ...").
bool parse_config(const std::string& text, Config* out, std::string* error);
bool load_config(const std::string& path, Config* out, std::string* error);
// Range checks and defaults (e.g. a tx/rx stream covering all channels when
// none were given). Returns false with a message if the config is unusable.
bool validate_config(Config* c, std::string* error);

}  // namespace dsv
