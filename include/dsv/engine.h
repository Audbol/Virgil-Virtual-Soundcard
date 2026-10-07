// The dsvd audio engine: media-clock tick, AES67 transmit/receive and the
// shared-memory soundcard that the OS drivers attach to.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "dsv/config.h"
#include "dsv/media_clock.h"
#include "dsv/net.h"
#include "dsv/rtp.h"
#include "dsv/sap.h"
#include "dsv/sdp.h"
#include "dsv/shm.h"
#include "dsv/shm_layout.h"

namespace dsv {

class Engine {
 public:
  explicit Engine(Config cfg);
  ~Engine();

  // Use a SAP service owned by the caller, so the directory of discovered
  // streams survives engine restarts. Call before start().
  void set_shared_sap(SapService* sap) { sap_shared_ = sap; }
  // Use a shared-memory segment owned by the caller. If it already holds a
  // soundcard with the same layout it is reused as is, so connected apps
  // keep running across engine restarts; otherwise it is recreated.
  void set_shared_memory(SharedMemory* shm) { shm_ext_ = shm; }

  // Optionally inject a clock (tests); otherwise built from the config.
  bool start(std::unique_ptr<ClockSource> clock = nullptr);
  void stop();
  bool running() const { return running_; }

  ShmHeader* header() { return hdr_; }
  const Config& config() const { return cfg_; }
  std::string status_line() const;

  // ---- Introspection for the control panel (safe from any thread) ----
  struct RxFlowStatus {
    StreamConfig cfg;
    std::string address;  // resolved group/port (from config or SAP)
    uint16_t port = 0;
    bool resolved = false;   // SAP name found / address known
    bool receiving = false;  // packets within the last second
  };
  std::vector<RxFlowStatus> rx_status() const;
  std::vector<SdpInfo> discovered() const;
  uint32_t interface_address() const { return iface_; }
  std::string grandmaster() const { return clock_ ? clock_->grandmaster() : std::string(); }
  // Peak level (linear) per channel since the previous call; resets them.
  void take_peaks(std::vector<float>* tx, std::vector<float>* rx);

 private:
  struct TxStream {
    StreamConfig cfg;
    UdpSocket sock;
    Endpoint dst;
    RtpHeader rtp;
    SdpInfo sdp;
    std::vector<uint8_t> packet;
  };
  struct RxStream {
    StreamConfig cfg;
    std::thread thread;
    std::mutex mutex;
    SdpInfo target;  // guarded by mutex
    bool have_target = false;
    std::atomic<uint32_t> generation{0};
    uint32_t source_addr = 0;
    std::atomic<int64_t> last_rx_ns{0};
  };

  void tick_loop();
  void rx_loop(RxStream* s);
  void housekeeping_loop();
  void mix_and_send(uint64_t block_start);
  void reclaim_clients(int64_t now);
  void clear_rings();
  SdpInfo make_sdp(const TxStream& t) const;

  Config cfg_;
  uint32_t iface_ = 0;
  SharedMemory shm_own_;
  SharedMemory* shm_ext_ = nullptr;
  SharedMemory& shm() { return shm_ext_ ? *shm_ext_ : shm_own_; }
  ShmHeader* hdr_ = nullptr;
  std::unique_ptr<ClockSource> clock_;
  std::unique_ptr<SapService> sap_;  // own instance when not shared
  SapService* sap_shared_ = nullptr;
  SapService* sap() const { return sap_shared_ ? sap_shared_ : sap_.get(); }
  std::vector<std::unique_ptr<TxStream>> tx_;
  std::vector<std::unique_ptr<RxStream>> rx_;
  std::vector<float> mix_;
  std::thread tick_thread_, hk_thread_;
  std::atomic<bool> running_{false};
  std::string last_gm_;
  // Peak meters: float bits of |sample|, max-accumulated by the audio
  // threads and drained by take_peaks().
  std::array<std::atomic<uint32_t>, kMaxChannels> tx_peak_{};
  std::array<std::atomic<uint32_t>, kMaxChannels> rx_peak_{};
};

}  // namespace dsv
