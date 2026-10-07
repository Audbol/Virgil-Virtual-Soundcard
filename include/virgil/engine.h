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
#include <string>
#include <thread>
#include <vector>

#include "virgil/config.h"
#include "virgil/media_clock.h"
#include "virgil/shm.h"
#include "virgil/shm_layout.h"

namespace virgil {

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
  bool dante_running() const { return dante_ != nullptr; }
  // False if the Dante stack was started and has since crashed or stopped.
  bool dante_healthy() const;
  // Peak level (linear) per channel since the previous call; resets them.
  void take_peaks(std::vector<float>* tx, std::vector<float>* rx);

  // Direct access to the Dante-side rings (tests).
  std::vector<int32_t>& dante_tx_ring() { return dtx_; }
  std::vector<int32_t>& dante_rx_ring() { return drx_; }

 private:
  void tick_loop();
  void reclaim_clients(int64_t now);
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
  std::vector<int32_t> dtx_, drx_;  // Dante-side rings (32-bit, interleaved)
  uint32_t dring_ = 0;
  std::vector<float> mix_;
  std::thread tick_thread_;
  std::atomic<bool> running_{false};
  // Peak meters: float bits of |sample|, max-accumulated by the tick thread
  // and drained by take_peaks().
  std::array<std::atomic<uint32_t>, kMaxChannels> tx_peak_{};
  std::array<std::atomic<uint32_t>, kMaxChannels> rx_peak_{};
};

}  // namespace virgil
