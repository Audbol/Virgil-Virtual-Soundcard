// Mapping between the local monotonic clock, PTP time and the Dante media
// clock (RTP timestamp = sample count since the PTP epoch, plus offset).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "virgil/shm_layout.h"

namespace virgil {

// Seconds between the PTP (TAI) and Unix (UTC) epochs, as of 2017-01-01.
constexpr int64_t kTaiUtcOffsetNs = 37LL * 1000000000LL;

struct ClockModel {
  int64_t base_local = 0;  // mono_ns()
  int64_t base_ptp = 0;    // PTP ns
  double ratio = 1.0;      // d(ptp) / d(local)

  int64_t ptp_at(int64_t local) const {
    return base_ptp + int64_t(double(local - base_local) * ratio);
  }
  int64_t local_at(int64_t ptp) const {
    return base_local + int64_t(double(ptp - base_ptp) / ratio);
  }
};

inline uint64_t ptp_ns_to_frames(int64_t ptp_ns, uint32_t rate) {
  const uint64_t ns = uint64_t(ptp_ns < 0 ? 0 : ptp_ns);
  return (ns / 1000000000ULL) * rate + (ns % 1000000000ULL) * rate / 1000000000ULL;
}

// Earliest PTP ns at which `frame` has started.
inline int64_t frames_to_ptp_ns(uint64_t frame, uint32_t rate) {
  return int64_t((frame / rate) * 1000000000ULL + ((frame % rate) * 1000000000ULL + rate - 1) / rate);
}

// Single-writer, many-reader container for a ClockModel without locks.
class ClockModelCell {
 public:
  void store(const ClockModel& m) {
    uint64_t r;
    std::memcpy(&r, &m.ratio, sizeof r);
    uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    local_.store(m.base_local, std::memory_order_relaxed);
    ptp_.store(m.base_ptp, std::memory_order_relaxed);
    ratio_.store(r, std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);
  }
  ClockModel load() const {
    for (;;) {
      uint32_t s0 = seq_.load(std::memory_order_acquire);
      if (s0 & 1) continue;
      ClockModel m;
      m.base_local = local_.load(std::memory_order_relaxed);
      m.base_ptp = ptp_.load(std::memory_order_relaxed);
      uint64_t r = ratio_.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq_.load(std::memory_order_relaxed) != s0) continue;
      std::memcpy(&m.ratio, &r, sizeof r);
      return m;
    }
  }

 private:
  std::atomic<uint32_t> seq_{0};
  std::atomic<int64_t> local_{0};
  std::atomic<int64_t> ptp_{0};
  std::atomic<uint64_t> ratio_{0x3ff0000000000000ULL};  // 1.0
};

class ClockSource {
 public:
  virtual ~ClockSource() = default;
  virtual ClockModel model() const = 0;
  virtual DaemonState state() const = 0;
  // Grandmaster identity formatted for SDP ts-refclk, empty if none.
  virtual std::string grandmaster() const { return {}; }
  virtual int64_t last_offset_ns() const { return 0; }
};

// Local oscillator only: PTP time := TAI derived from the wall clock at start.
class FreeRunClock : public ClockSource {
 public:
  FreeRunClock();
  ClockModel model() const override { return m_; }
  DaemonState state() const override { return kStateFreeRun; }

 private:
  ClockModel m_;
};

}  // namespace virgil
