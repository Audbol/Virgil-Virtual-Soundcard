// IEEE 1588-2008 (PTPv2) ordinary clock over UDP/IPv4, AES67 media profile.
//
// Slave: E2E delay mechanism, two-step and one-step masters, best-master
// selection from Announce messages, software timestamps (kernel receive
// timestamps on Linux). Optional fallback master when no grandmaster exists.
//
// Dante devices speak PTPv1 natively; with AES67 mode enabled in Dante
// Controller they bridge their clock onto PTPv2 domain 0, which this follows.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "dsv/media_clock.h"
#include "dsv/net.h"

namespace dsv {

namespace ptp {

constexpr uint16_t kEventPort = 319;
constexpr uint16_t kGeneralPort = 320;
constexpr uint32_t kPrimaryGroup = 0xE0000181;  // 224.0.1.129
constexpr size_t kHeaderBytes = 34;

enum MessageType : uint8_t {
  kSync = 0x0,
  kDelayReq = 0x1,
  kFollowUp = 0x8,
  kDelayResp = 0x9,
  kAnnounce = 0xB,
};

using ClockId = std::array<uint8_t, 8>;

struct PortId {
  ClockId clock{};
  uint16_t port = 0;
  bool operator==(const PortId& o) const { return clock == o.clock && port == o.port; }
  bool operator!=(const PortId& o) const { return !(*this == o); }
};

struct Timestamp {
  uint64_t seconds = 0;  // 48 bits on the wire
  uint32_t nanoseconds = 0;
  int64_t ns() const { return int64_t(seconds) * 1000000000LL + nanoseconds; }
  static Timestamp from_ns(int64_t ns) {
    return {uint64_t(ns / 1000000000LL), uint32_t(ns % 1000000000LL)};
  }
};

struct Header {
  uint8_t type = 0;
  uint8_t version = 2;
  uint16_t length = 0;
  uint8_t domain = 0;
  uint16_t flags = 0;
  int64_t correction = 0;  // scaled ns (ns * 2^16)
  PortId source;
  uint16_t sequence = 0;
  uint8_t control = 0;
  int8_t log_interval = 0;

  bool two_step() const { return (flags & 0x0200) != 0; }
  int64_t correction_ns() const { return correction >> 16; }
};

struct Announce {
  int16_t utc_offset = 37;
  uint8_t priority1 = 128;
  uint8_t clock_class = 248;
  uint8_t clock_accuracy = 0xFE;
  uint16_t variance = 0xFFFF;
  uint8_t priority2 = 128;
  ClockId grandmaster{};
  uint16_t steps_removed = 0;
  uint8_t time_source = 0xA0;  // internal oscillator
};

bool parse_header(const uint8_t* p, size_t len, Header* h);
size_t write_header(uint8_t* p, const Header& h);
Timestamp read_timestamp(const uint8_t* p);
void write_timestamp(uint8_t* p, const Timestamp& t);
bool parse_announce(const uint8_t* p, size_t len, Announce* a);
size_t write_announce(uint8_t* p, const Header& h, const Announce& a);
// IEEE 1588 dataset comparison: < 0 if a is the better master.
int compare_announce(const Announce& a, const PortId& a_port, const Announce& b,
                     const PortId& b_port);
std::string format_clock_id(const ClockId& id);  // XX-XX-XX-XX-XX-XX-XX-XX

}  // namespace ptp

// PI servo turning (local, master) samples into a phase-continuous ClockModel.
class PiServo {
 public:
  enum Result { kInit, kStep, kTracking, kOutlier };
  Result sample(int64_t local_ns, int64_t master_ns);
  ClockModel model() const { return m_; }
  void reset_to(const ClockModel& m) {
    m_ = m;
    init_ = true;
    count_ = 0;
  }
  bool locked() const { return locked_; }
  int64_t last_error_ns() const { return last_err_; }

  double kp = 0.03;
  double ki = 0.0006;
  int64_t step_threshold_ns = 1000000;  // 1 ms

 private:
  ClockModel m_;
  bool init_ = false;
  bool locked_ = false;
  int count_ = 0;
  int good_ = 0;
  double drift_ppb_ = 0;
  double rms_ = 0;
  int64_t last_local_ = 0;
  int64_t last_err_ = 0;
  int64_t first_local_ = 0, first_master_ = 0;
};

class PtpClock : public ClockSource {
 public:
  struct Options {
    uint32_t interface_addr = 0;
    uint8_t domain = 0;
    bool master_capable = true;  // become GM if nobody else is around
    uint8_t priority1 = 250;     // lose against any real grandmaster
    uint8_t priority2 = 250;
  };

  ~PtpClock() override { stop(); }
  bool start(const Options& o);
  void stop();

  ClockModel model() const override { return cell_.load(); }
  DaemonState state() const override { return DaemonState(state_.load()); }
  std::string grandmaster() const override;
  int64_t last_offset_ns() const override { return offset_ns_.load(); }

 private:
  void run();
  void handle_event(const uint8_t* p, size_t n, int64_t rx_ns, const Endpoint& from);
  void handle_general(const uint8_t* p, size_t n, int64_t rx_ns);
  void process_sync_pair(int64_t t1, int64_t t2);
  void send_delay_req();
  void master_duties(int64_t now);
  void become_master();
  void select_master(int64_t now);

  Options opt_;
  UdpSocket event_, general_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint32_t> state_{kStateFreeRun};
  std::atomic<int64_t> offset_ns_{0};
  ClockModelCell cell_;
  PiServo servo_;
  ptp::PortId self_;

  // Best foreign master.
  bool have_master_ = false;
  ptp::PortId master_;
  ptp::Announce master_announce_;
  int64_t master_seen_ns_ = 0;
  int64_t start_ns_ = 0;
  mutable std::mutex gm_mutex_;
  std::string gm_string_;

  // Sync / Follow_Up pairing.
  uint16_t sync_seq_ = 0;
  int64_t sync_rx_ns_ = 0;
  int64_t sync_correction_ns_ = 0;
  bool sync_pending_ = false;
  int64_t last_t1_ = 0, last_t2_ = 0;
  bool have_pair_ = false;

  // Delay measurement.
  uint16_t delay_seq_ = 0;
  int64_t delay_tx_ns_ = 0;
  bool delay_pending_ = false;
  int64_t next_delay_req_ns_ = 0;
  int64_t delays_[9] = {};
  int delay_count_ = 0;
  int64_t mean_path_delay_ = 0;

  // Master mode.
  bool is_master_ = false;
  uint16_t m_sync_seq_ = 0, m_announce_seq_ = 0;
  int64_t next_sync_ns_ = 0, next_announce_ns_ = 0;
};

}  // namespace dsv
