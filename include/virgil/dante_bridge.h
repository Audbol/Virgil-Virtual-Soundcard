// C interface of the Virgil <-> Inferno bridge (bridge/src/lib.rs).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VgDanteConfig {
  const char* name;      // device name shown in Dante Controller (<= 31 chars)
  const char* bind_ip;   // IPv4 address of the Dante network interface
  uint32_t sample_rate;  // 44100 / 48000 / 88200 / 96000
  uint32_t tx_channels;  // channels this computer sends to the network
  uint32_t rx_channels;  // channels this computer receives
  uint32_t tx_latency_ns;     // latency demanded from receivers of our channels
  uint32_t rx_latency_ns;     // our receive latency
  uint32_t tx_send_delay_ns;  // frame f is read from tx_ring and sent at media time f + this
  // Interleaved 32-bit sample rings indexed by media frame (frame & (frames-1)),
  // owned by the caller and valid until vg_dante_stop() returns.
  int32_t* tx_ring;
  uint32_t tx_ring_frames;
  int32_t* rx_ring;
  uint32_t rx_ring_frames;
  // Monotonic clock the PTP overlay refers to (virgil::mono_ns).
  int64_t (*mono_ns)(void);
  // Log sink (level 0 error .. 3 debug); may be NULL.
  void (*log)(int level, const char* message);
  // Channel names, tx_channels / rx_channels entries (or NULL). A non-empty
  // entry sets the name; others keep the device's saved name ("TX n" / "RX n").
  const char* const* tx_names;
  const char* const* rx_names;
  // First of four alternative control ports (ARC, CMC, flow control, info),
  // or 0 for the standard Dante ports. Dante Controller finds them by mDNS.
  uint16_t alt_port;
} VgDanteConfig;

typedef struct VgDanteRxStats {
  uint64_t packets;          // audio packets received
  uint64_t late_packets;     // arrived after their receive latency (too late to play)
  uint64_t gaps;             // holes in a stream (lost packets)
  uint64_t missing_samples;  // samples per channel that never arrived
  uint64_t max_age_samples;  // oldest packet on arrival since the last call
} VgDanteRxStats;

// Starts the Dante device. Returns an opaque handle, or NULL on failure.
void* vg_dante_start(const VgDanteConfig* config);
// Software version Dante Controller shows for the device.
void vg_dante_set_version(unsigned major, unsigned minor, unsigned patch);
// Why the last vg_dante_start() failed (NULL if unknown).
const char* vg_dante_last_error(void);
// Publishes the PTP clock: ptp_ns = t + shift + (t - last_sync) * freq_scale,
// t being mono_ns(). Call whenever the servo updates.
void vg_dante_set_clock(int64_t last_sync, int64_t shift, double freq_scale);
// The PTP clock leader's EUI-64 identity (8 bytes), or NULL when there is
// none. Dante Controller shows it (and the sync state) in its clock status.
void vg_dante_set_clock_master(const uint8_t* id);
// Called from a Dante thread when a channel is renamed from the network
// (Dante Controller): is_tx, 0-based index, new name. NULL removes it.
void vg_dante_set_rename_callback(void (*cb)(int is_tx, uint32_t index, const char* name));
// Current name of a channel of the running device into buf (len bytes).
// Returns 0 if the device has not published its names yet.
int vg_dante_channel_name(int is_tx, uint32_t index, char* buf, uint32_t len);
// Receive counters since the device started (max_age resets per call).
void vg_dante_rx_stats(VgDanteRxStats* out);
// Stops the device and releases the rings.
// 1 while running normally, 0 after a crash of any of its threads.
int vg_dante_healthy(void* handle);

void vg_dante_stop(void* handle);

#ifdef __cplusplus
}
#endif
