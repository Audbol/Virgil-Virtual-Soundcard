// The virgild audio engine: PTP media clock, the shared-memory soundcard the
// OS drivers attach to, and the Dante device (Inferno) on the network.
//
//   apps -> drivers -> shm client rings --tick: mix--> Dante TX ring -> Inferno -> network
//   apps <- drivers <- shm capture ring <--tick: copy-- Dante RX ring <- Inferno <- network
//
// Every ring is indexed by the same absolute media frame (PTP time x rate),
// so the hand-over is a copy at the same index.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "virgil/config.h"
#include "virgil/media_clock.h"
#include "virgil/shm.h"
#include "virgil/shm_layout.h"

namespace virgil {

// Counters for the diagnostics view, sampled once a second. Totals are since
// the engine started, except the receive counters (since the process did).
struct EngineCounters {
  uint32_t state = 0;  // DaemonState
  std::string grandmaster;
  uint64_t late_ticks = 0;     // engine woke up late (computer busy)
  uint64_t overdue_ticks = 0;  // so late that audio missed its slot (a dropout)
  // Runs of consecutive overdue ticks (one stall and the catch-up after it
  // are one episode), long stalls included.
  uint64_t overdue_episodes = 0;
  // Stalls longer than the resync limit on an unchanged clock: the engine
  // skipped ahead (audio paused) without realigning the drivers.
  uint64_t stalls = 0;
  int64_t max_late_ns = 0;     // latest tick wake-up since the last sample
  uint64_t resyncs = 0;        // media clock realigned while running (clock steps)
  // Frames of received audio that Inferno wrote after the engine had handed
  // them to the apps for the last time: arrived too late and lost.
  uint64_t rx_lost_late_samples = 0;
  int64_t max_offset_ns = 0;   // largest |PTP offset| since the last sample
  uint64_t rx_packets = 0, rx_late_packets = 0, rx_gaps = 0, rx_missing_samples = 0;
  // rx_late_packets: older than the receive latency on arrival (Inferno). The
  // engine's second copy usually still saves them: rx_lost_late_samples
  // counts what was actually lost.
  uint64_t rx_max_age_samples = 0;  // since the last sample
  // Dante sending thread woke up after the transmit latency (audio to the
  // network dropped); since the process started.
  uint64_t tx_late_wakeups = 0, tx_dropped_samples = 0, tx_clock_jumps = 0;
  uint64_t tx_max_lag_samples = 0;  // since the last sample
  // Near misses of the sending thread (not dropouts): worst lag of any
  // wakeup since the last sample, and wakeups later than 0.5 / 1 / 2 / 4 ms
  // (cumulative, since the process started).
  uint64_t tx_max_wakeup_lag_samples = 0;
  uint64_t tx_lag_over[4] = {};
  struct App {
    uint32_t slot = 0, pid = 0;
    std::string name;
    bool active = false;
    uint64_t underrun_ticks = 0;  // ticks the app had not written in time
    uint64_t late_frames = 0;     // frames the app wrote after their deadline
  };
  std::vector<App> apps;
};

class Engine {
 public:
  explicit Engine(Config cfg);
  ~Engine();

  // Use a shared-memory segment owned by the caller. If it already holds a
  // soundcard with the same layout it is reused, so connected apps keep
  // running across engine restarts; otherwise it is recreated.
  void set_shared_memory(SharedMemory* shm) { shm_ext_ = shm; }
  // Run without the Dante device (tests of the soundcard/clock path).
  void set_dante_enabled(bool on) { dante_enabled_ = on; }

  // Optionally inject a clock (tests); otherwise built from the config.
  bool start(std::unique_ptr<ClockSource> clock = nullptr);
  void stop();
  bool running() const { return running_; }

  ShmHeader* header() { return hdr_; }
  const Config& config() const { return cfg_; }
  std::string status_line() const;

  // ---- Introspection for the control panel (safe from any thread) ----
  uint32_t interface_address() const { return iface_; }
  std::string grandmaster() const { return clock_ ? clock_->grandmaster() : std::string(); }
  // The clock in use (a PtpClock when clock = ptp); null when stopped.
  const ClockSource* clock_source() const { return clock_.get(); }
  bool dante_running() const { return dante_ != nullptr; }
  // First Dante control port in use (4440 = the standard ports).
  uint16_t control_port_base() const { return alt_port_ ? alt_port_ : 4440; }
  // False if the Dante stack was started and has since crashed or stopped.
  bool dante_healthy() const;
  // Peak level (linear) per channel since the previous call; resets them.
  void take_peaks(std::vector<float>* tx, std::vector<float>* rx);
  // Diagnostics counters (resets the per-sample maxima).
  EngineCounters counters();
  // Current channel names (as shown in Dante Controller).
  std::vector<std::string> channel_names(bool tx) const;
  // Picks up names the device published or that were changed from the
  // network, and hands them to the drivers. Call about once a second.
  void sync_channel_names();
  // Releases slots of apps that died, reports the clock leader to Inferno.
  // Call about once a second, not from the tick thread.
  void housekeeping();
  struct Rename {
    bool tx;
    uint32_t index;
    std::string name;
  };
  // Channels renamed from the network (Dante Controller) since the last call,
  // for the daemon to save to its configuration.
  std::vector<Rename> take_renames();

  // Direct access to the Dante-side rings (tests).
  std::vector<int32_t>& dante_tx_ring() { return dtx_; }
  std::vector<int32_t>& dante_rx_ring() { return drx_; }

 private:
  void tick_loop();
  void reclaim_clients(int64_t now);
  void report_clock_master();
  void clear_rings();
  static void on_clock(const ClockModel& m, void* self);

  Config cfg_;
  uint32_t iface_ = 0;
  SharedMemory shm_own_;
  SharedMemory* shm_ext_ = nullptr;
  SharedMemory& shm() { return shm_ext_ ? *shm_ext_ : shm_own_; }
  ShmHeader* hdr_ = nullptr;
  std::unique_ptr<ClockSource> clock_;
  bool dante_enabled_ = true;
  void* dante_ = nullptr;
  uint16_t alt_port_ = 0;  // 0 = standard Dante ports
  std::vector<int32_t> dtx_, drx_;  // Dante-side rings (32-bit, interleaved)
  uint32_t dring_ = 0;
  std::vector<float> mix_;
  std::thread tick_thread_;
  std::atomic<bool> running_{false};
  // Peak meters: float bits of |sample|, max-accumulated by the tick thread
  // and drained by take_peaks().
  std::array<std::atomic<uint32_t>, kMaxChannels> tx_peak_{};
  std::array<bool, kMaxTxClients> client_stalled_{};  // housekeeping() only
  bool reported_master_ = false;  // housekeeping() only
  uint8_t reported_id_[8] = {};
  std::array<std::atomic<uint32_t>, kMaxChannels> rx_peak_{};
  static void on_rename(int is_tx, uint32_t index, const char* name);
  void publish_names_locked();
  mutable std::mutex names_mutex_;
  std::vector<std::string> tx_names_, rx_names_;  // guarded by names_mutex_
  std::vector<Rename> renames_;                   // guarded by names_mutex_

  // Diagnostics.
  std::atomic<uint64_t> resyncs_{0};
  std::atomic<uint64_t> overdue_ticks_{0};
  std::atomic<uint64_t> overdue_episodes_{0};
  std::atomic<uint64_t> stalls_{0};
  std::atomic<int64_t> max_late_ns_{0};
  std::atomic<uint64_t> rx_lost_late_{0};
  std::atomic<int64_t> max_offset_ns_{0};
  std::array<std::atomic<uint64_t>, kMaxTxClients> underrun_ticks_{};
  std::array<uint32_t, kMaxTxClients> underrun_pid_{};  // tick thread only
};

}  // namespace virgil
