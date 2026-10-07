// PTPv1 (IEEE 1588-2002) ordinary clock over UDP/IPv4: the clock protocol
// native Dante devices use (subdomain "_DFLT").
//
// Follower: E2E delay mechanism, two-step ("assist") and one-step masters,
// best master chosen from the properties carried in Sync messages, software
// timestamps (kernel receive timestamps on Linux). Like Dante Virtual
// Soundcard, Virgil normally never becomes master; `master_capable` exists
// for networks with only virtual devices (and for tests).
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

namespace ptp1 {

constexpr uint16_t kEventPort = 319;
constexpr uint16_t kGeneralPort = 320;
constexpr uint32_t kDefaultGroup = 0xE0000181;  // 224.0.1.129 (subdomain _DFLT)
constexpr size_t kHeaderBytes = 40;
constexpr size_t kSyncBytes = 124;      // Sync and Delay_Req
constexpr size_t kFollowUpBytes = 52;
constexpr size_t kDelayRespBytes = 60;

enum Control : uint8_t { kSync = 0, kDelayReq = 1, kFollowUp = 2, kDelayResp = 3 };
enum MessageType : uint8_t { kEventMessage = 1, kGeneralMessage = 2 };
constexpr uint8_t kFlagAssist = 0x08;   // two-step: precise time follows in Follow_Up

using Uuid = std::array<uint8_t, 6>;

struct PortId {
  Uuid uuid{};
  uint16_t port = 0;
  bool operator==(const PortId& o) const { return uuid == o.uuid && port == o.port; }
  bool operator!=(const PortId& o) const { return !(*this == o); }
};

struct Header {
  char subdomain[16] = {'_', 'D', 'F', 'L', 'T'};
  uint8_t message_type = kEventMessage;
  PortId source;
  uint16_t sequence = 0;
  uint8_t control = kSync;
  uint8_t flags = 0;  // second flags octet (LI61, LI59, BC, ASSIST, ...)
};

// Grandmaster / clock properties carried in Sync (and echoed in Delay_Req).
struct ClockProps {
  Uuid gm_uuid{};
  uint16_t gm_port = 0;
  uint16_t gm_sequence = 0;
  uint8_t stratum = 255;
  char identifier[4] = {'D', 'F', 'L', 'T'};
  int16_t variance = 0;
  bool preferred = false;
  bool boundary_clock = false;
  int8_t sync_interval = 0;  // log2 seconds
};

struct Timestamp {
  uint32_t seconds = 0;
  uint32_t nanoseconds = 0;
  int64_t ns() const { return int64_t(seconds) * 1000000000LL + nanoseconds; }
  static Timestamp from_ns(int64_t ns) {
    return {uint32_t(uint64_t(ns) / 1000000000ULL), uint32_t(uint64_t(ns) % 1000000000ULL)};
  }
};

bool parse_header(const uint8_t* p, size_t len, Header* h);
void write_header(uint8_t* p, const Header& h);
Timestamp read_timestamp(const uint8_t* p);
void write_timestamp(uint8_t* p, const Timestamp& t);

// Sync / Delay_Req body (bytes 40..123).
bool parse_sync(const uint8_t* p, size_t len, Timestamp* origin, ClockProps* props);
size_t write_sync(uint8_t* p, const Header& h, const Timestamp& origin, const ClockProps& props,
                  const PortId& parent);
// Follow_Up (bytes 40..51).
bool parse_follow_up(const uint8_t* p, size_t len, uint16_t* associated_seq, Timestamp* precise);
size_t write_follow_up(uint8_t* p, const Header& h, uint16_t associated_seq, const Timestamp& t);
// Delay_Resp (bytes 40..59).
bool parse_delay_resp(const uint8_t* p, size_t len, Timestamp* receipt, PortId* requester,
                      uint16_t* requester_seq);
size_t write_delay_resp(uint8_t* p, const Header& h, const Timestamp& receipt,
                        const PortId& requester, uint16_t requester_seq);

// Best master: < 0 if a is better than b (stratum, preferred, variance, uuid).
int compare_masters(const ClockProps& a, const PortId& ap, const ClockProps& b, const PortId& bp);
std::string format_uuid(const Uuid& u);  // 00:1d:c1:12:34:56

}  // namespace ptp1

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
    std::string subdomain = "_DFLT";
    bool master_capable = false;  // only for networks without Dante hardware
    uint8_t stratum = 254;        // as master: loses against real devices
  };

  ~PtpClock() override { stop(); }
  bool start(const Options& o);
  void stop();

  ClockModel model() const override { return cell_.load(); }
  DaemonState state() const override { return DaemonState(state_.load()); }
  std::string grandmaster() const override;
  int64_t last_offset_ns() const override { return offset_ns_.load(); }

  // Called whenever the clock model changes (Virgil feeds Inferno from it).
  void set_listener(void (*fn)(const ClockModel&, void*), void* ctx) {
    listener_ = fn;
    listener_ctx_ = ctx;
  }

 private:
  void run();
  void handle_event(const uint8_t* p, size_t n, int64_t rx_ns);
  void handle_general(const uint8_t* p, size_t n);
  void consider_master(const ptp1::PortId& src, const ptp1::ClockProps& props, int64_t now);
  void process_sync_pair(int64_t t1, int64_t t2);
  void send_delay_req();
  void master_duties(int64_t now);
  void publish(const ClockModel& m);

  Options opt_;
  ptp1::Header proto_;  // template header (subdomain, our identity)
  UdpSocket event_, general_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint32_t> state_{kStateFreeRun};
  std::atomic<int64_t> offset_ns_{0};
  ClockModelCell cell_;
  PiServo servo_;
  ptp1::PortId self_;
  void (*listener_)(const ClockModel&, void*) = nullptr;
  void* listener_ctx_ = nullptr;

  // Current master.
  bool have_master_ = false;
  ptp1::PortId master_;
  ptp1::ClockProps master_props_;
  int64_t master_seen_ns_ = 0;
  int64_t start_ns_ = 0;
  mutable std::mutex gm_mutex_;
  std::string gm_string_;

  // Sync / Follow_Up pairing.
  uint16_t sync_seq_ = 0;
  int64_t sync_rx_ns_ = 0;
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
  uint16_t m_sync_seq_ = 0;
  int64_t next_sync_ns_ = 0;
};

}  // namespace dsv
