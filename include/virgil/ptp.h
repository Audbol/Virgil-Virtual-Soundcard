// PTPv1 (IEEE 1588-2002) ordinary clock over UDP/IPv4: the clock protocol
// Dante devices use (subdomain "_DFLT").
//
// Follower: E2E delay mechanism, two-step ("assist") and one-step masters,
// best master chosen from the properties carried in Sync messages, software
// timestamps (kernel receive stamps on Linux, macOS and Windows where
// available; kernel send stamps on Linux, else a bracket around sendto()). Like Dante Virtual
// Soundcard, Virgil normally never becomes master; `master_capable` exists
// for networks with only virtual devices (and for tests).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "virgil/media_clock.h"
#include "virgil/net.h"
#include "virgil/platform.h"

namespace virgil {

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
  // master_ns = t1 + delay_ns: the frequency seed is measured on t1 alone,
  // so a path-delay estimate still settling cannot bias it.
  Result sample(int64_t local_ns, int64_t master_ns, int64_t delay_ns = 0);
  ClockModel model() const { return m_; }
  // Continue from model m (keeps phase); the first sample after this starts
  // a fresh frequency measurement.
  void reset_to(const ClockModel& m) {
    m_ = m;
    init_ = true;
    count_ = 0;
    good_ = 0;
    big_ = 0;
    locked_ = false;
    rms_ = 0;
    rejects_ = 0;
    nbig_ = 0;
    seeded_ = false;
    reseeded_ = false;
    age_ = 0;
    aerr_ = 0;
    calm_since_ = 0;
    hist_n_ = 0;
    first_local_ = last_local_ = 0;
  }
  bool locked() const { return locked_; }
  // A new master (possibly on another timeline): the next error beyond the
  // threshold steps at once instead of waiting for confirmation windows.
  void allow_step() {
    count_ = 0;
    nbig_ = 0;
  }
  int64_t last_error_ns() const { return last_err_; }
  double drift_ppb() const { return drift_ppb_; }
  // Smoothed |error| (time constant ~5 s), what the lock decision uses.
  double filtered_error_ns() const { return aerr_; }

  double kp = 0.03;
  double ki = 0.0006;
  int64_t step_threshold_ns = 1000000;  // 1 ms
  // Once tracking, an error beyond the threshold steps only after this many
  // consecutive windows agree on it (a passing timestamp disturbance must
  // not step the clock and then step it back).
  int confirm_windows = 5;
  // Gain schedule: start with the fast gains, blend geometrically to kp/ki
  // between update fast_updates and 3 * fast_updates.
  double kp_fast = 0.1;
  double ki_fast = 0.003;
  int fast_updates = 60;
  // Lock: smoothed |error| below lock_error_ns, frequency moving less than
  // lock_drift_ppb over the last ~4 s, both for lock_hold_s seconds.
  double lock_error_ns = 100000;
  double lock_drift_ppb = 1000;
  double lock_hold_s = 5;

 private:
  void hold(int64_t local, int64_t est);
  ClockModel m_;
  bool init_ = false;
  bool locked_ = false;
  int count_ = 0;
  int good_ = 0;
  int big_ = 0;  // unused (kept for layout); see big_errs_
  int nbig_ = 0;  // consecutive windows beyond the step threshold
  int64_t big_errs_[8] = {};
  int rejects_ = 0;  // consecutive samples rejected by the envelope gate
  bool seeded_ = false;  // frequency seeded from a raw measurement
  bool reseeded_ = false;  // and refined once over a longer baseline
  double drift_ppb_ = 0;
  double rms_ = 0;
  int64_t last_local_ = 0;
  int64_t last_err_ = 0;
  int64_t first_local_ = 0, first_master_ = 0;
  int64_t first_delay_ = 0;
  int age_ = 0;              // tracking updates since reset_to / initial step
  double aerr_ = 0;          // EWMA of |err|
  int64_t calm_since_ = 0;   // local ns the lock conditions began to hold; 0 = not
  // Frequency history, one entry per >= 1 s, for the settled check.
  int64_t hist_local_[8] = {};
  double hist_drift_[8] = {};
  int hist_n_ = 0;
  void update_lock(int64_t local, int64_t err, double dt);
};

class PtpClock : public ClockSource {
 public:
  struct Options {
    uint32_t interface_addr = 0;
    std::string subdomain = "_DFLT";
    bool master_capable = false;  // only for networks without Dante hardware
    uint8_t stratum = 254;        // as master: loses against real devices
    // Sync messages per servo update: the least-delayed of each group is
    // used. Software receive timestamps are only ever late, never early, so
    // this drops the scheduling hiccups (1 s at Dante's 4 Sync/s).
    int sync_window = 4;
  };

  ~PtpClock() override { stop(); }
  bool start(const Options& o);
  void stop();

  ClockModel model() const override { return cell_.load(); }
  DaemonState state() const override { return DaemonState(state_.load()); }
  std::string grandmaster() const override;
  int64_t last_offset_ns() const override { return offset_ns_.load(); }

  // Read-only listening state for the control panel (safe from any thread).
  // Distinct hosts heard sending PTP traffic (any version / subdomain).
  int senders_seen() const { return senders_seen_.load(); }
  // A master was followed at some point since start().
  bool master_ever_heard() const { return master_ever_.load(); }
  // Seconds since start() or since the master was lost, while neither
  // following a master nor acting as one; 0 otherwise.
  double seconds_without_master() const {
    const int64_t since = no_master_since_ns_.load();
    if (!since) return 0.0;
    const int64_t d = mono_ns() - since;
    return d > 0 ? double(d) / 1e9 : 0.0;
  }

  // Called whenever the clock model changes (Virgil feeds Inferno from it).
  void set_listener(void (*fn)(const ClockModel&, void*), void* ctx) {
    listener_ = fn;
    listener_ctx_ = ctx;
  }

 private:
  void run();
  void handle_event(const uint8_t* p, size_t n, int64_t rx_ns);
  // Logs the first packet from each sender (diagnostics for clocking issues).
  void note_sender(const Endpoint& from, const uint8_t* p, size_t n, bool event_port);
  uint32_t senders_[32] = {};
  int sender_count_ = 0;
  bool warned_no_master_ = false;
  std::atomic<int> senders_seen_{0};
  std::atomic<bool> master_ever_{false};
  std::atomic<int64_t> no_master_since_ns_{0};  // mono ns; 0 = following or leading
  void handle_general(const uint8_t* p, size_t n);
  void consider_master(const ptp1::PortId& src, const ptp1::ClockProps& props, int64_t now);
  void process_sync_pair(int64_t t1, int64_t t2);
  void send_delay_req();
  // Takes the Delay_Req send time (t3) from the mono_ns() stamps read just
  // before and after sendto() and, when nonzero, a kernel TX timestamp.
  // Discards the sample when the send was preempted (bracket too wide).
  void accept_delay_tx(int64_t before, int64_t after, int64_t kernel_ns);
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
  int64_t best_t1_ = 0, best_t2_ = 0;  // least-delayed Sync in the current window
  int64_t last_best_ms_ = 0;            // its t2 - t1, from the last full window
  int64_t last_best_t2_ = 0;            // and when it arrived
  bool have_best_ms_ = false;
  int window_count_ = 0;

  // Delay measurement.
  uint16_t delay_seq_ = 0;
  int64_t delay_tx_ns_ = 0;
  bool delay_pending_ = false;
  int64_t next_delay_req_ns_ = 0;
  int64_t delays_[9] = {};
  int delay_count_ = 0;
  int64_t mean_path_delay_ = 0;
  int64_t tx_span_min_ = 0;    // narrowest before/after bracket seen around sendto()
  int tx_wide_run_ = 0;        // consecutive brackets discarded as too wide
  int tx_kernel_tries_ = 0;    // kernel TX stamps: misses left before giving up
  uint64_t tx_kernel_ = 0, tx_bracket_ = 0, tx_discarded_ = 0;  // counts, for the log
  int64_t next_diag_ns_ = 0;   // once-a-minute debug line
  uint64_t jitter_state_ = 0;  // Delay_Req timing jitter (seeded per instance)

  // Master mode.
  bool is_master_ = false;
  uint16_t m_sync_seq_ = 0;
  int64_t next_sync_ns_ = 0;
};

}  // namespace virgil
