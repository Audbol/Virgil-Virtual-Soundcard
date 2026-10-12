// Sample-rate conversion with a continuously variable ratio, and a clock
// bridge that uses it to carry audio between two free-running clocks (a
// Windows audio device and Virgil's Dante media clock).
//
// Portable C++, no allocation after init(), no locks: Resampler is used by one
// thread; AdaptiveBridge by one producer and one consumer thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace virgil {

// Kaiser-windowed sinc interpolator, polyphase table with linear interpolation
// between phases (any ratio, changed per call). Output frame n of a stream is
// the input signal at input frame position n * ratio (accumulated), so the
// resampler adds no delay of its own beyond needing half a kernel of input
// ahead.
//
// Quality: about 90 dB stopband, flat to ~83% of the lower Nyquist frequency
// (20 kHz at 48 kHz); the cutoff follows the largest ratio expected, so
// downsampling (e.g. 96 -> 48 kHz) does not alias.
class Resampler {
 public:
  // ratio = input frames per output frame (in_rate / out_rate).
  // max_out_frames: the largest produce() call; max_deviation: how far the
  // ratio may stray from nominal (drift correction).
  bool init(unsigned channels, double nominal_ratio, size_t max_out_frames = 4096, double max_deviation = 0.01);
  // Forget all input; the next output starts from silence history.
  void reset();

  unsigned channels() const { return ch_; }
  unsigned taps() const { return taps_; }
  double nominal_ratio() const { return nominal_; }

  // Input frames still to push() before produce(out_frames, ratio) can
  // deliver all frames.
  size_t needed(size_t out_frames, double ratio) const;
  // Appends interleaved input (nullptr = silence). False if it does not fit
  // (more than max_out_frames * ratio buffered).
  bool push(const float* in, size_t frames);
  // Writes up to out_frames interleaved frames; returns how many (fewer only
  // when not enough input was pushed).
  size_t produce(float* out, size_t out_frames, double ratio);
  // Input frames at or after the read position (fractional).
  double unread() const { return double(size_) - pos_; }

 private:
  void compact();

  static constexpr unsigned kPhases = 256;
  unsigned ch_ = 0, taps_ = 0, half_ = 0;
  double nominal_ = 1;
  std::vector<float> table_;  // (kPhases + 1) rows of taps_
  std::vector<float> coef_;   // scratch: one interpolated row
  std::vector<float> buf_;    // cap_ frames of ch_
  size_t cap_ = 0, size_ = 0;
  double pos_ = 0;  // read position in buf_ (frames)
};

// Carries audio from a producer on one clock to a consumer on another: a
// lock-free FIFO, a Resampler, and a PI controller that trims the resampling
// ratio so the FIFO level stays put (clock drift, e.g. +-200 ppm between a
// sound card and the Dante clock), plus the fixed ratio between the two
// nominal rates (44.1 -> 48 kHz).
//
// The level is measured from time stamps, not from what happens to be in the
// FIFO: the producer says when its last frame was produced, and the level
// counts the frames it produced since then (at the nominal rate, for at most
// max_extrapolate_s). That removes the saw tooth of bursty producers (10 ms
// packets) from the control loop.
//
// When the FIFO runs dry (the source stopped delivering, e.g. nothing plays)
// the consumer gets silence and the bridge waits until target_s has built up
// again. The drift estimate (the controller's integral) is kept.
struct BridgeConfig {
  unsigned channels = 2;
  double in_rate = 48000, out_rate = 48000;
  double target_s = 0.040;           // FIFO level to keep
  double capacity_s = 0.5;           // FIFO size; above target + slack_s the excess is dropped
  double slack_s = 0.150;
  double max_extrapolate_s = 0.030;  // producer silent longer: stop counting
  double max_adjust = 0.005;         // ratio trim limit (+-0.5 %)
  double filter_s = 1.0;             // level filter time constant
  double loop_hz = 0.025;            // controller bandwidth (natural frequency / 2 pi)
  size_t max_pull = 4096;            // largest pull() / push() in frames
};

class AdaptiveBridge {
 public:
  enum State : uint32_t { kPriming = 0, kRunning = 1 };

  bool init(const BridgeConfig& cfg);
  const BridgeConfig& config() const { return cfg_; }

  // ---- producer thread
  // Interleaved frames (nullptr = silence); end_ns: host time (mono_ns) at
  // which the last of them was produced. Returns frames accepted (fewer when
  // the FIFO is full).
  size_t push(const float* in, size_t frames, int64_t end_ns);

  // ---- consumer thread
  // Always writes `frames` frames; returns false when that was silence
  // (priming or an underrun). now_ns: host time this output is due.
  bool pull(float* out, size_t frames, int64_t now_ns);

  // ---- any thread (statistics)
  State state() const { return State(state_.load(std::memory_order_relaxed)); }
  double adjust_ppm() const { return adjust_ppm_.load(std::memory_order_relaxed); }
  double level_ms() const { return level_ms_.load(std::memory_order_relaxed); }
  uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
  uint64_t overflows() const { return overflows_.load(std::memory_order_relaxed); }
  uint64_t frames_in() const { return w_.load(std::memory_order_relaxed); }
  // Producer time stamp of the latest push (0 = none yet).
  int64_t last_push_ns() const { return stamp_ns_.load(std::memory_order_relaxed); }

 private:
  size_t fifo_to_resampler(size_t frames);  // consumer
  bool read_stamp(uint64_t* frames, int64_t* ns) const;

  BridgeConfig cfg_;
  Resampler rs_;
  std::vector<float> ring_;   // cap_ frames
  uint64_t cap_ = 0, mask_ = 0;
  uint64_t target_ = 0, high_ = 0;
  double nominal_ = 1, ratio_ = 1;
  double kp_ = 0, ki_ = 0;

  alignas(64) std::atomic<uint64_t> w_{0};  // frames written (producer)
  std::atomic<uint32_t> stamp_seq_{0};
  std::atomic<uint64_t> stamp_frames_{0};
  std::atomic<int64_t> stamp_ns_{0};
  alignas(64) std::atomic<uint64_t> r_{0};  // frames read (consumer)

  // Consumer-only controller state.
  bool fresh_ = true;
  double level_f_ = 0, setpoint_ = 0, integ_ = 0;
  int64_t last_ns_ = 0;
  uint64_t stamp_frames_cache_ = 0;
  int64_t stamp_ns_cache_ = 0;

  std::atomic<uint32_t> state_{kPriming};
  std::atomic<double> adjust_ppm_{0}, level_ms_{0};
  std::atomic<uint64_t> underruns_{0}, overflows_{0};
};

}  // namespace virgil
