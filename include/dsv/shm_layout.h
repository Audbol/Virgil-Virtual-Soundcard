// Shared-memory contract between the dsvd daemon and the host-side drivers
// (ALSA ioplug, CoreAudio AudioServerPlugIn, ASIO). Everything here must stay
// binary compatible between processes built by different compilers, so it
// only uses fixed-width integers and lock-free std::atomic of those.
//
// Rings are indexed by *absolute media frame* (PTP-derived sample clock), not
// by read/write cursors. That turns the network jitter buffer, the client
// mixer and the driver clocks into the same thing: a frame for media time t
// lives in slot (t & (ring_frames - 1)) and every party agrees on t.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dsv {

constexpr uint32_t kShmMagic = 0x31565344;  // "DSV1"
constexpr uint32_t kShmVersion = 1;
constexpr uint32_t kMaxChannels = 64;
constexpr uint32_t kMaxTxClients = 8;
constexpr size_t kShmHeaderBytes = 4096;

#if defined(_WIN32)
// The service creates a Global section visible to every session; a dsvd
// started from an unprivileged console falls back to the Local namespace.
constexpr const char* kDefaultShmName = "Global\\DSVSoundcard";
constexpr const char* kFallbackShmName = "Local\\DSVSoundcard";
#else
constexpr const char* kDefaultShmName = "/dsv-soundcard";
#endif

enum DaemonState : uint32_t {
  kStateStopped = 0,
  kStateFreeRun = 1,  // running on local clock, no PTP grandmaster
  kStatePtpLocked = 2,
  kStatePtpMaster = 3,
};

// One per producer of playback audio (an ALSA pcm, the CoreAudio device, an
// ASIO host). The daemon sums all active slots, so several apps can play at
// once without a mixer lock.
struct alignas(64) ClientSlot {
  std::atomic<uint32_t> pid;     // owner process, 0 = free
  std::atomic<uint32_t> active;  // 1 while the owner is producing audio
  std::atomic<int64_t> heartbeat_ns;
  char name[48];
};

struct alignas(64) ShmHeader {
  // Immutable after the daemon creates the segment.
  uint32_t magic;
  uint32_t version;
  uint64_t total_bytes;
  uint32_t sample_rate;
  uint32_t tx_channels;        // apps -> network (playback)
  uint32_t rx_channels;        // network -> apps (capture)
  uint32_t ring_frames;        // power of two
  uint32_t period_frames;      // daemon tick == AES67 packet time
  uint32_t rx_latency_frames;  // capture reads lag the media clock by this
  uint32_t tx_lead_frames;     // playback writes lead the media clock by this
  uint32_t reserved0;
  char device_name[64];

  // Liveness.
  alignas(64) std::atomic<uint32_t> state;
  std::atomic<int64_t> heartbeat_ns;  // daemon mono_ns() at last tick

  // Media clock anchor, guarded by a seqlock (odd = write in progress).
  // media_frame(host_ns) = anchor_frame + (host_ns - anchor_ns) * frames_per_ns
  alignas(64) std::atomic<uint32_t> clock_seq;
  std::atomic<int64_t> anchor_ns;
  std::atomic<uint64_t> anchor_frame;
  std::atomic<uint64_t> frames_per_ns_bits;  // double, bit-cast
  std::atomic<uint64_t> now_frames;          // media frame of the last tick

  // Statistics (monotonic counters, informational).
  alignas(64) std::atomic<uint64_t> tx_packets;
  std::atomic<uint64_t> rx_packets;
  std::atomic<uint64_t> rx_lost;
  std::atomic<uint64_t> rx_late;
  std::atomic<uint64_t> clock_steps;
  std::atomic<uint64_t> late_ticks;
  std::atomic<int64_t> ptp_offset_ns;

  ClientSlot clients[kMaxTxClients];
};

static_assert(sizeof(ShmHeader) <= kShmHeaderBytes, "header too large");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "need lock-free 64-bit atomics");
static_assert(std::atomic<int64_t>::is_always_lock_free, "need lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "need lock-free 32-bit atomics");

inline size_t tx_ring_floats(const ShmHeader* h) {
  return size_t(h->ring_frames) * h->tx_channels;
}
inline size_t rx_ring_floats(const ShmHeader* h) {
  return size_t(h->ring_frames) * h->rx_channels;
}

inline size_t shm_total_bytes(uint32_t ring_frames, uint32_t tx_channels, uint32_t rx_channels) {
  return kShmHeaderBytes +
         sizeof(float) * size_t(ring_frames) * (size_t(tx_channels) * kMaxTxClients + rx_channels);
}

inline float* tx_ring(ShmHeader* h, uint32_t slot) {
  return reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(h) + kShmHeaderBytes) +
         tx_ring_floats(h) * slot;
}
inline float* rx_ring(ShmHeader* h) {
  return reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(h) + kShmHeaderBytes) +
         tx_ring_floats(h) * kMaxTxClients;
}
inline const float* rx_ring(const ShmHeader* h) { return rx_ring(const_cast<ShmHeader*>(h)); }

// ---- Clock anchor ---------------------------------------------------------

struct ClockAnchor {
  int64_t host_ns = 0;   // local monotonic time (see platform.h mono_ns)
  uint64_t frame = 0;    // media frame at host_ns
  double frames_per_ns = 0;

  double frame_at(int64_t ns) const { return double(frame) + double(ns - host_ns) * frames_per_ns; }
  int64_t host_ns_at(double f) const {
    return host_ns + int64_t((f - double(frame)) / frames_per_ns);
  }
};

inline void publish_anchor(ShmHeader* h, const ClockAnchor& a) {
  uint64_t bits;
  std::memcpy(&bits, &a.frames_per_ns, sizeof bits);
  uint32_t seq = h->clock_seq.load(std::memory_order_relaxed);
  h->clock_seq.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  h->anchor_ns.store(a.host_ns, std::memory_order_relaxed);
  h->anchor_frame.store(a.frame, std::memory_order_relaxed);
  h->frames_per_ns_bits.store(bits, std::memory_order_relaxed);
  h->clock_seq.store(seq + 2, std::memory_order_release);
}

inline bool read_anchor(const ShmHeader* h, ClockAnchor* out) {
  for (int tries = 0; tries < 64; ++tries) {
    uint32_t s0 = h->clock_seq.load(std::memory_order_acquire);
    if (s0 & 1) continue;
    ClockAnchor a;
    a.host_ns = h->anchor_ns.load(std::memory_order_relaxed);
    a.frame = h->anchor_frame.load(std::memory_order_relaxed);
    uint64_t bits = h->frames_per_ns_bits.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (h->clock_seq.load(std::memory_order_relaxed) != s0) continue;
    std::memcpy(&a.frames_per_ns, &bits, sizeof bits);
    if (a.frames_per_ns <= 0) return false;
    *out = a;
    return true;
  }
  return false;
}

}  // namespace dsv
