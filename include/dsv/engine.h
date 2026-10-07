// The dsvd audio engine: media-clock tick, AES67 transmit/receive and the
// shared-memory soundcard that the OS drivers attach to.
#pragma once

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

  // Optionally inject a clock (tests); otherwise built from the config.
  bool start(std::unique_ptr<ClockSource> clock = nullptr);
  void stop();
  bool running() const { return running_; }

  ShmHeader* header() { return hdr_; }
  const Config& config() const { return cfg_; }
  std::string status_line() const;

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
  SharedMemory shm_;
  ShmHeader* hdr_ = nullptr;
  std::unique_ptr<ClockSource> clock_;
  std::unique_ptr<SapService> sap_;
  std::vector<std::unique_ptr<TxStream>> tx_;
  std::vector<std::unique_ptr<RxStream>> rx_;
  std::vector<float> mix_;
  std::thread tick_thread_, hk_thread_;
  std::atomic<bool> running_{false};
  std::string last_gm_;
};

}  // namespace dsv
