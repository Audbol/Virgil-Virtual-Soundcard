#include "virgil/client.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "virgil/platform.h"

namespace virgil {

std::string default_shm_name() {
  if (const char* e = std::getenv("VIRGIL_SHM_NAME"); e && *e) return e;
  return kDefaultShmName;
}

// Open the live soundcard among the base name's generations (see
// shm_generation_name): prefer one whose daemon is ticking.
bool Client::open(const std::string& name) {
  close();
  const std::string base = name.empty() ? default_shm_name() : name;
  int best = -1;
  for (int gen = 0; gen < kShmGenerations; ++gen) {
    SharedMemory probe;
    if (!probe.open(shm_generation_name(base, gen)) || probe.size() < kShmHeaderBytes) continue;
    const auto* h = static_cast<const ShmHeader*>(probe.data());
    if (h->magic != kShmMagic) continue;
    const bool live = h->state.load() != kStateStopped && mono_ns() - h->heartbeat_ns.load() < 2000000000LL;
    if (best < 0) best = gen;
    if (live) {
      best = gen;
      break;
    }
  }
  bool ok = best >= 0 && shm_.open(shm_generation_name(base, best));
#if defined(_WIN32)
  if (!ok && name.empty()) ok = shm_.open(kFallbackShmName);
#endif
  if (!ok) return false;
  auto* h = static_cast<ShmHeader*>(shm_.data());
  if (shm_.size() < kShmHeaderBytes) {
    shm_.close();
    return false;
  }
  // Any local user can write the header: take one copy of the layout and
  // validate and keep exactly that (reading the fields again after the
  // checks let a racing writer slip in an unchecked geometry).
  struct Layout {
    uint32_t magic, version;
    uint64_t total_bytes;
    uint32_t rate, tx, rx, ring, period, rx_lat, tx_lead;
  } v;
  {
    volatile const ShmHeader* vh = h;
    v.magic = vh->magic;
    v.version = vh->version;
    v.total_bytes = vh->total_bytes;
    v.rate = vh->sample_rate;
    v.tx = vh->tx_channels;
    v.rx = vh->rx_channels;
    v.ring = vh->ring_frames;
    v.period = vh->period_frames;
    v.rx_lat = vh->rx_latency_frames;
    v.tx_lead = vh->tx_lead_frames;
  }
  if (v.magic != kShmMagic || v.version != kShmVersion || v.total_bytes > shm_.size() || v.ring == 0 ||
      (v.ring & (v.ring - 1)) != 0 || v.tx > kMaxChannels || v.rx > kMaxChannels ||
      shm_total_bytes(v.ring, v.tx, v.rx) > shm_.size() || v.rate == 0 || v.rate > 384000 || v.period == 0 ||
      v.period >= v.ring / 4 || v.rx_lat >= v.ring / 4 || v.tx_lead >= v.ring / 4) {
    shm_.close();
    return false;
  }
  hdr_ = h;
  // Keep the validated layout; never index with values re-read from the header.
  rate_ = v.rate;
  txch_ = v.tx;
  rxch_ = v.rx;
  ring_ = v.ring;
  period_ = v.period;
  rx_lat_ = v.rx_lat;
  tx_lead_ = v.tx_lead;
  mask_ = ring_ - 1;
  rx_ = rx_ring_at(h, ring_, txch_);
  return true;
}

void Client::close() {
  release_tx_slot();
  hdr_ = nullptr;
  tx_ = nullptr;
  rx_ = nullptr;
  shm_.close();
}

bool Client::daemon_alive(int64_t timeout_ns) const {
  if (!hdr_) return false;
  if (hdr_->state.load(std::memory_order_acquire) == kStateStopped) return false;
  return mono_ns() - hdr_->heartbeat_ns.load(std::memory_order_acquire) < timeout_ns;
}

bool Client::heartbeat_fresh(int64_t timeout_ns) const {
  if (!hdr_) return false;
  return mono_ns() - hdr_->heartbeat_ns.load(std::memory_order_acquire) < timeout_ns;
}

void Client::clear_pending_tx() {
  if (!tx_) return;
  std::memset(tx_, 0, size_t(ring_) * txch_ * sizeof(float));
}

bool Client::frame_now(double* out) const {
  ClockAnchor a;
  if (!hdr_ || !anchor(&a)) return false;
  *out = a.frame_at(mono_ns());
  return true;
}

bool Client::acquire_tx_slot(const char* name) {
  if (!hdr_) return false;
  if (slot_ >= 0) return true;
  const uint32_t me = process_id();
  for (uint32_t i = 0; i < kMaxTxClients; ++i) {
    ClientSlot& s = hdr_->clients[i];
    uint32_t expected = 0;
    if (!s.pid.compare_exchange_strong(expected, me, std::memory_order_acq_rel)) continue;
    std::memset(s.name, 0, sizeof s.name);
    if (name) {
      // At most sizeof - 1 bytes, cut on a UTF-8 character boundary.
      size_t n = strnlen(name, sizeof s.name);
      if (n > sizeof s.name - 1) {
        n = sizeof s.name - 1;
        while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) --n;
      }
      std::memcpy(s.name, name, n);
    }
    s.heartbeat_ns.store(mono_ns(), std::memory_order_relaxed);
    s.written_until.store(0, std::memory_order_relaxed);
    s.late_frames.store(0, std::memory_order_relaxed);
    slot_ = int(i);
    tx_ = tx_ring_at(hdr_, ring_, txch_, i);
    std::memset(tx_, 0, size_t(ring_) * txch_ * sizeof(float));
    s.active.store(0, std::memory_order_release);
    return true;
  }
  return false;
}

void Client::release_tx_slot() {
  if (!hdr_ || slot_ < 0) return;
  ClientSlot& s = hdr_->clients[slot_];
  s.active.store(0, std::memory_order_release);
  s.pid.store(0, std::memory_order_release);
  slot_ = -1;
  tx_ = nullptr;
}

void Client::set_tx_active(bool on) {
  if (slot_ < 0) return;
  touch();
  // A (re)started stream has written nothing yet: not late until it has.
  hdr_->clients[slot_].written_until.store(0, std::memory_order_release);
  hdr_->clients[slot_].active.store(on ? 1 : 0, std::memory_order_release);
}

void Client::touch() {
  if (slot_ >= 0) hdr_->clients[slot_].heartbeat_ns.store(mono_ns(), std::memory_order_relaxed);
}

uint32_t Client::write_tx(uint64_t frame, const float* src, uint32_t frames,
                          uint32_t src_channels) {
  if (!tx_) return frames;
  const uint32_t ch = txch_;
  const uint32_t n = src_channels < ch ? src_channels : ch;
  const uint64_t horizon = tx_horizon();
  const uint32_t skip =
      frame >= horizon ? 0 : uint32_t(std::min<uint64_t>(frames, horizon - frame));
  for (uint32_t f = skip; f < frames; ++f) {
    float* d = tx_frame(frame + f);
    const float* s = src + size_t(f) * src_channels;
    for (uint32_t c = 0; c < n; ++c) d[c] = s[c];
  }
  note_late(skip);
  if (skip < frames) note_written(frame + frames);
  return skip;
}

void Client::read_rx(uint64_t frame, float* dst, uint32_t frames, uint32_t dst_channels) const {
  const uint32_t ch = rxch_;
  const uint32_t n = dst_channels < ch ? dst_channels : ch;
  for (uint32_t f = 0; f < frames; ++f) {
    const float* s = rx_frame(frame + f);
    float* d = dst + size_t(f) * dst_channels;
    uint32_t c = 0;
    for (; c < n; ++c) d[c] = s[c];
    for (; c < dst_channels; ++c) d[c] = 0.f;
  }
}

}  // namespace virgil
