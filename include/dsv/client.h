// Driver-side view of the daemon. Linked into the ALSA plugin, the CoreAudio
// HAL plug-in and the ASIO driver. Nothing in the audio-thread methods
// allocates, locks or makes a syscall.
#pragma once

#include <cstdint>
#include <string>

#include "dsv/shm.h"
#include "dsv/shm_layout.h"

namespace dsv {

class Client {
 public:
  Client() = default;
  ~Client() { close(); }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Name defaults to $DSV_SHM_NAME, then kDefaultShmName.
  bool open(const std::string& shm_name = std::string());
  void close();
  bool is_open() const { return hdr_ != nullptr; }

  // True if the daemon ticked within the last `timeout_ns`.
  bool daemon_alive(int64_t timeout_ns = 250000000) const;

  const ShmHeader* header() const { return hdr_; }
  uint32_t sample_rate() const { return hdr_->sample_rate; }
  uint32_t tx_channels() const { return hdr_->tx_channels; }
  uint32_t rx_channels() const { return hdr_->rx_channels; }
  uint32_t ring_frames() const { return hdr_->ring_frames; }
  uint32_t period_frames() const { return hdr_->period_frames; }
  uint32_t rx_latency_frames() const { return hdr_->rx_latency_frames; }
  uint32_t tx_lead_frames() const { return hdr_->tx_lead_frames; }

  bool anchor(ClockAnchor* out) const { return read_anchor(hdr_, out); }
  // Interpolated media frame "now". Returns false if no clock yet.
  bool frame_now(double* out) const;

  // Playback mixing slot. Must be held to use write_tx / tx_frame.
  bool acquire_tx_slot(const char* name);
  void release_tx_slot();
  bool has_tx_slot() const { return slot_ >= 0; }
  void set_tx_active(bool on);
  void touch();  // refresh slot heartbeat; call at least every few hundred ms

  // Interleaved float I/O at absolute media frame positions. Channels beyond
  // the device count are dropped (tx) or zero-filled (rx).
  void write_tx(uint64_t frame, const float* src, uint32_t frames, uint32_t src_channels);
  void read_rx(uint64_t frame, float* dst, uint32_t frames, uint32_t dst_channels) const;

  // Per-frame access for drivers that convert formats on the fly. The pointer
  // addresses tx_channels() / rx_channels() contiguous floats.
  float* tx_frame(uint64_t frame) {
    return tx_ + size_t(frame & mask_) * hdr_->tx_channels;
  }
  const float* rx_frame(uint64_t frame) const {
    return rx_ + size_t(frame & mask_) * hdr_->rx_channels;
  }

 private:
  SharedMemory shm_;
  ShmHeader* hdr_ = nullptr;
  float* tx_ = nullptr;
  const float* rx_ = nullptr;
  uint64_t mask_ = 0;
  int slot_ = -1;
};

std::string default_shm_name();

}  // namespace dsv
