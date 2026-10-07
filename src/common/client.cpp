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

bool Client::open(const std::string& name) {
  close();
  if (!shm_.open(name.empty() ? default_shm_name() : name)) {
#if defined(_WIN32)
    if (!name.empty() || !shm_.open(kFallbackShmName)) return false;
#else
    return false;
#endif
  }
  auto* h = static_cast<ShmHeader*>(shm_.data());
  if (shm_.size() < kShmHeaderBytes || h->magic != kShmMagic || h->version != kShmVersion ||
      h->total_bytes > shm_.size() || h->ring_frames == 0 ||
      (h->ring_frames & (h->ring_frames - 1)) != 0 || h->tx_channels > kMaxChannels ||
      h->rx_channels > kMaxChannels ||
      shm_total_bytes(h->ring_frames, h->tx_channels, h->rx_channels) > shm_.size()) {
    shm_.close();
    return false;
  }
  hdr_ = h;
  mask_ = h->ring_frames - 1;
  rx_ = rx_ring(h);
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
    if (name) std::strncpy(s.name, name, sizeof s.name - 1);
    s.heartbeat_ns.store(mono_ns(), std::memory_order_relaxed);
    slot_ = int(i);
    tx_ = tx_ring(hdr_, i);
    std::memset(tx_, 0, tx_ring_floats(hdr_) * sizeof(float));
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
  hdr_->clients[slot_].active.store(on ? 1 : 0, std::memory_order_release);
}

void Client::touch() {
  if (slot_ >= 0) hdr_->clients[slot_].heartbeat_ns.store(mono_ns(), std::memory_order_relaxed);
}

uint32_t Client::write_tx(uint64_t frame, const float* src, uint32_t frames,
                          uint32_t src_channels) {
  if (!tx_) return frames;
  const uint32_t ch = hdr_->tx_channels;
  const uint32_t n = src_channels < ch ? src_channels : ch;
  const uint64_t horizon = tx_horizon();
  const uint32_t skip =
      frame >= horizon ? 0 : uint32_t(std::min<uint64_t>(frames, horizon - frame));
  for (uint32_t f = skip; f < frames; ++f) {
    float* d = tx_frame(frame + f);
    const float* s = src + size_t(f) * src_channels;
    for (uint32_t c = 0; c < n; ++c) d[c] = s[c];
  }
  return skip;
}

void Client::read_rx(uint64_t frame, float* dst, uint32_t frames, uint32_t dst_channels) const {
  const uint32_t ch = hdr_->rx_channels;
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
