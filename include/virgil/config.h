// Daemon configuration (INI-style file, see config/virgil.conf.example).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace virgil {

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
  // Dante control ports: "auto" (standard 4440/8800/4455/8700 unless other
  // Dante software on this computer, e.g. Dante Controller, already uses
  // them), "standard", or the first of four alternative ports.
  std::string ports = "auto";
  // false: Virgil is stopped (Stop in Virgil Control): no audio engine, off
  // the Dante network, all its ports free, until started again.
  bool enabled = true;

  // Channel names ([channel_names] tx1 = Kick ...); "" = the device's own
  // ("TX n" / "RX n", or a name given in Dante Controller). Index 0 = channel 1.
  std::vector<std::string> tx_names, rx_names;
  std::string tx_name(uint32_t i) const { return i < tx_names.size() ? tx_names[i] : std::string(); }
  std::string rx_name(uint32_t i) const { return i < rx_names.size() ? rx_names[i] : std::string(); }

  // Messages about ignored legacy settings, for the log.
  std::vector<std::string> notes;

  uint32_t us_to_frames(uint32_t us) const {
    return uint32_t((uint64_t(sample_rate) * us + 500000) / 1000000);
  }
  uint32_t period_frames() const { return us_to_frames(tick_us); }
  // How far capture reads trail the clock (drivers): Inferno already applies
  // the Dante receive latency; this only covers the hand-over ticks.
  uint32_t rx_latency_frames() const { return 3 * period_frames(); }
  // How long after its media time a mixed frame is handed to the network:
  // the tick that mixes it ends one tick later, plus room for one packet and
  // scheduling jitter. tx_latency_us must cover this plus the network.
  uint32_t tx_send_delay_us() const { return 2 * tick_us + 500; }
  uint32_t tx_lead_frames() const {
    return tx_lead_us ? us_to_frames(tx_lead_us) : 2 * period_frames();
  }
};

// Parse INI text. On failure returns false and fills `error` ("line N: ...").
bool parse_config(const std::string& text, Config* out, std::string* error);
bool load_config(const std::string& path, Config* out, std::string* error);
// Range checks and derived defaults. Returns false with a message if unusable.
bool validate_config(Config* c, std::string* error);
// Why `name` cannot be a channel name, or "" if it can.
std::string channel_name_problem(const std::string& name);
// Default name of a channel (1-based), as the device shows it unnamed.
std::string default_channel_name(bool tx, uint32_t number);
// Replace the configuration file at `path` with `text` safely (see .cpp).
// Hold config_file_mutex() while calling it; the editors below take it.
bool write_config_file(const std::string& path, const std::string& text, std::string* error);
std::mutex& config_file_mutex();
// Set (or with an empty value, remove) `key` in `[section]` of the file at
// `path`, keeping everything else (comments, layout, other keys).
bool set_config_value(const std::string& path, const std::string& section, const std::string& key,
                      const std::string& value, std::string* error);
// Set (or with an empty name, remove) one channel name in the file at `path`,
// keeping everything else. Used when a channel is renamed in Dante Controller.
bool save_channel_name(const std::string& path, bool tx, uint32_t index, const std::string& name,
                       std::string* error);

// Dante names must be unique on the network, and every installation starts
// out as "Virgil": that stock name becomes Virgil-<host name>.
extern const char* const kStockDeviceName;
std::string host_device_name();
// Rewrite the `name` line of [device] in the file at `path` if it is still
// the stock name, keeping everything else. Returns false if not changed.
bool replace_stock_name(const std::string& path, const std::string& name, std::string* error);
// Serialise back to INI text that parse_config() reads.
std::string format_config(const Config& c);

}  // namespace virgil
