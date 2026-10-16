#include "virgil/resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace virgil {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Modified Bessel function of the first kind, order 0 (series).
double bessel_i0(double x) {
  double sum = 1, term = 1;
  const double q = x * x / 4;
  for (int k = 1; k < 64; ++k) {
    term *= q / (double(k) * double(k));
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

}  // namespace

// ---- Resampler -------------------------------------------------------------------

bool Resampler::init(unsigned channels, double nominal_ratio, size_t max_out_frames, double max_deviation) {
  if (channels == 0 || channels > 64 || !(nominal_ratio > 0.01) || !(nominal_ratio < 16) || max_out_frames == 0)
    return false;
  ch_ = channels;
  nominal_ = nominal_ratio;
  // Cutoff (fraction of the input Nyquist frequency): the -6 dB point sits
  // below the lower of the two Nyquist frequencies so the transition band
  // ends near it; 32 zero crossings each side, Kaiser beta 8.6 (~90 dB).
  const double max_ratio = nominal_ratio * (1 + std::max(0.0, max_deviation));
  const double fc = 0.91 / std::max(1.0, max_ratio);
  const unsigned zero_crossings = 32;
  half_ = unsigned(std::ceil(zero_crossings / fc));
  taps_ = 2 * half_;
  const double beta = 8.6, i0b = bessel_i0(beta);
  table_.assign(size_t(kPhases + 1) * taps_, 0.f);
  for (unsigned p = 0; p <= kPhases; ++p) {
    const double d = double(p) / kPhases;
    double sum = 0;
    std::vector<double> row(taps_);
    for (unsigned j = 0; j < taps_; ++j) {
      // Distance (input frames) from tap j to the output position.
      const double x = double(int(j) - int(half_) + 1) - d;
      const double u = x / double(half_);
      double w = 0;
      if (std::fabs(u) < 1) w = bessel_i0(beta * std::sqrt(1 - u * u)) / i0b;
      const double a = kPi * fc * x;
      const double s = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
      row[j] = fc * s * w;
      sum += row[j];
    }
    // Unity gain at DC for every phase (no ripple from the phase grid).
    for (unsigned j = 0; j < taps_; ++j) table_[size_t(p) * taps_ + j] = float(row[j] / sum);
  }
  coef_.assign(taps_, 0.f);
  cap_ = size_t(std::ceil(double(max_out_frames) * max_ratio)) + 2 * size_t(taps_) + 64;
  buf_.assign(cap_ * ch_, 0.f);
  reset();
  return true;
}

void Resampler::reset() {
  // half - 1 frames of silence before the read position, which then sits on
  // the next frame pushed.
  size_ = half_ ? half_ - 1 : 0;
  std::fill(buf_.begin(), buf_.begin() + long(size_ * ch_), 0.f);
  pos_ = double(size_);
}

size_t Resampler::needed(size_t out_frames, double ratio) const {
  if (out_frames == 0 || !taps_) return 0;
  // Same accumulation as produce(), so the two agree to the bit.
  double p = pos_;
  for (size_t n = 1; n < out_frames; ++n) p += ratio;
  const size_t need = size_t(std::floor(p)) + half_ + 1;
  return need > size_ ? need - size_ : 0;
}

void Resampler::compact() {
  const double fl = std::floor(pos_);
  if (fl < double(half_)) return;
  const size_t drop = size_t(fl) - (half_ - 1);
  if (drop == 0 || drop > size_) return;
  std::memmove(buf_.data(), buf_.data() + drop * ch_, (size_ - drop) * ch_ * sizeof(float));
  size_ -= drop;
  pos_ -= double(drop);
}

bool Resampler::push(const float* in, size_t frames) {
  if (!taps_) return false;
  if (size_ + frames > cap_) compact();
  if (size_ + frames > cap_) return false;
  float* d = buf_.data() + size_ * ch_;
  if (in) std::memcpy(d, in, frames * ch_ * sizeof(float));
  else std::fill(d, d + frames * ch_, 0.f);
  size_ += frames;
  return true;
}

size_t Resampler::produce(float* out, size_t out_frames, double ratio) {
  if (!taps_) return 0;
  const unsigned T = taps_;
  size_t n = 0;
  for (; n < out_frames; ++n) {
    const double fl = std::floor(pos_);
    const size_t i = size_t(fl);
    if (i + half_ + 1 > size_) break;  // not enough input
    const double pf = (pos_ - fl) * kPhases;
    unsigned ph = unsigned(pf);
    if (ph >= kPhases) ph = kPhases - 1;
    const float a = float(pf - ph);
    const float* t0 = table_.data() + size_t(ph) * T;
    const float* t1 = t0 + T;
    for (unsigned j = 0; j < T; ++j) coef_[j] = t0[j] + a * (t1[j] - t0[j]);
    const float* x = buf_.data() + (i + 1 - half_) * ch_;
    float* y = out + n * ch_;
    if (ch_ == 2) {
      float l = 0, r = 0;
      for (unsigned j = 0; j < T; ++j) {
        l += coef_[j] * x[2 * j];
        r += coef_[j] * x[2 * j + 1];
      }
      y[0] = l;
      y[1] = r;
    } else if (ch_ == 1) {
      float s = 0;
      for (unsigned j = 0; j < T; ++j) s += coef_[j] * x[j];
      y[0] = s;
    } else if (ch_ >= 5) {
      // Audio interfaces (up to 64 channels): taps outer, channels inner, so
      // the inner loop vectorizes (8 channels 1.8x, 64 channels 4.4x faster).
      // Each channel still sums in tap order: the same result.
      std::fill(y, y + ch_, 0.f);
      for (unsigned j = 0; j < T; ++j) {
        const float cj = coef_[j];
        const float* xj = x + size_t(j) * ch_;
        for (unsigned c = 0; c < ch_; ++c) y[c] += cj * xj[c];
      }
    } else {
      for (unsigned c = 0; c < ch_; ++c) {
        float s = 0;
        for (unsigned j = 0; j < T; ++j) s += coef_[j] * x[size_t(j) * ch_ + c];
        y[c] = s;
      }
    }
    pos_ += ratio;
  }
  return n;
}

// ---- AdaptiveBridge -------------------------------------------------------------

bool AdaptiveBridge::init(const BridgeConfig& cfg) {
  if (cfg.channels == 0 || !(cfg.in_rate > 0) || !(cfg.out_rate > 0) || cfg.max_pull == 0) return false;
  cfg_ = cfg;
  nominal_ = cfg.in_rate / cfg.out_rate;
  ratio_ = nominal_;
  if (!rs_.init(cfg.channels, nominal_, cfg.max_pull, 2 * cfg.max_adjust)) return false;
  // FIFO: a power of two holding capacity_s (at least target + slack + one
  // pull and a producer burst).
  const double need = std::max(cfg.capacity_s, cfg.target_s + cfg.slack_s + 0.05) * cfg.in_rate +
                      double(cfg.max_pull) * (nominal_ + 1);
  cap_ = 1024;
  while (double(cap_) < need) cap_ <<= 1;
  mask_ = cap_ - 1;
  ring_.assign(size_t(cap_) * cfg.channels, 0.f);
  target_ = uint64_t(std::llround(cfg.target_s * cfg.in_rate));
  high_ = target_ + uint64_t(std::llround(cfg.slack_s * cfg.in_rate));
  // PI on the level error in seconds: d(err)/dt = drift - adjust. Natural
  // frequency w, damping 1: kp = 2w, ki = w^2.
  const double w = 2 * kPi * cfg.loop_hz;
  kp_ = 2 * w;
  ki_ = w * w;
  w_.store(0);
  r_.store(0);
  stamp_seq_.store(0);
  stamp_frames_.store(0);
  stamp_ns_.store(0);
  fresh_ = true;
  level_f_ = setpoint_ = integ_ = 0;
  last_ns_ = 0;
  stamp_frames_cache_ = 0;
  stamp_ns_cache_ = 0;
  state_.store(kPriming);
  adjust_ppm_.store(0);
  level_ms_.store(0);
  underruns_.store(0);
  overflows_.store(0);
  return true;
}

size_t AdaptiveBridge::push(const float* in, size_t frames, int64_t end_ns) {
  if (!cap_) return 0;
  const unsigned ch = cfg_.channels;
  const uint64_t w = w_.load(std::memory_order_relaxed);
  const uint64_t r = r_.load(std::memory_order_acquire);
  const uint64_t used = w - r;
  const uint64_t room = used >= cap_ ? 0 : cap_ - used;
  const size_t n = size_t(std::min<uint64_t>(frames, room));
  if (n < frames) overflows_.fetch_add(1, std::memory_order_relaxed);
  size_t done = 0;
  while (done < n) {
    const uint64_t at = (w + done) & mask_;
    const size_t run = size_t(std::min<uint64_t>(n - done, cap_ - at));
    float* d = ring_.data() + at * ch;
    if (in) std::memcpy(d, in + done * ch, run * ch * sizeof(float));
    else std::fill(d, d + run * ch, 0.f);
    done += run;
  }
  w_.store(w + n, std::memory_order_release);
  // Seqlock-published stamp: frames written so far, and when the last was.
  const uint32_t s = stamp_seq_.load(std::memory_order_relaxed);
  stamp_seq_.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  stamp_frames_.store(w + n, std::memory_order_relaxed);
  stamp_ns_.store(end_ns, std::memory_order_relaxed);
  stamp_seq_.store(s + 2, std::memory_order_release);
  return n;
}

bool AdaptiveBridge::read_stamp(uint64_t* frames, int64_t* ns) const {
  for (int tries = 0; tries < 8; ++tries) {
    const uint32_t s0 = stamp_seq_.load(std::memory_order_acquire);
    if (s0 & 1) continue;
    const uint64_t f = stamp_frames_.load(std::memory_order_relaxed);
    const int64_t t = stamp_ns_.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (stamp_seq_.load(std::memory_order_relaxed) != s0) continue;
    *frames = f;
    *ns = t;
    return true;
  }
  return false;
}

size_t AdaptiveBridge::fifo_to_resampler(size_t frames) {
  const unsigned ch = cfg_.channels;
  const uint64_t r = r_.load(std::memory_order_relaxed);
  size_t done = 0;
  while (done < frames) {
    const uint64_t at = (r + done) & mask_;
    const size_t run = size_t(std::min<uint64_t>(frames - done, cap_ - at));
    if (!rs_.push(ring_.data() + at * ch, run)) break;
    done += run;
  }
  r_.store(r + done, std::memory_order_release);
  return done;
}

bool AdaptiveBridge::pull(float* out, size_t frames, int64_t now_ns) {
  const unsigned ch = cfg_.channels;
  auto silence = [&] {
    std::fill(out, out + frames * ch, 0.f);
    return false;
  };
  if (!cap_ || frames == 0) return silence();
  if (frames > cfg_.max_pull) {
    // Larger than planned for: in pieces.
    bool any = false;
    for (size_t done = 0; done < frames;) {
      const size_t n = std::min(cfg_.max_pull, frames - done);
      any |= pull(out + done * ch, n, now_ns);
      done += n;
    }
    return any;
  }
  const double dt = last_ns_ ? std::min(0.1, std::max(0.0, double(now_ns - last_ns_) * 1e-9)) : 0.0;
  last_ns_ = now_ns;

  const uint64_t w = w_.load(std::memory_order_acquire);
  uint64_t r = r_.load(std::memory_order_relaxed);
  uint64_t avail = w - r;

  if (state_.load(std::memory_order_relaxed) == kPriming) {
    if (avail < target_) {
      level_ms_.store(double(avail) * 1000.0 / cfg_.in_rate, std::memory_order_relaxed);
      return silence();
    }
    // Enough built up: start at the target level (older audio is dropped).
    r = w - target_;
    r_.store(r, std::memory_order_release);
    avail = target_;
    rs_.reset();
    fresh_ = true;
    state_.store(kRunning, std::memory_order_relaxed);
  }

  if (avail > high_) {
    // The consumer fell far behind (or the producer burst): drop to target.
    r = w - target_;
    r_.store(r, std::memory_order_release);
    avail = target_;
    fresh_ = true;
    overflows_.fetch_add(1, std::memory_order_relaxed);
  }

  const size_t need = rs_.needed(frames, ratio_);
  if (need > avail) {
    // Ran dry: silence until target_ has built up again. The drift estimate
    // (integ_) stays.
    underruns_.fetch_add(1, std::memory_order_relaxed);
    state_.store(kPriming, std::memory_order_relaxed);
    return silence();
  }
  fifo_to_resampler(need);
  const size_t got = rs_.produce(out, frames, ratio_);
  if (got < frames) std::fill(out + got * ch, out + frames * ch, 0.f);

  // ---- controller
  uint64_t sf;
  int64_t sn;
  if (read_stamp(&sf, &sn)) {
    stamp_frames_cache_ = sf;
    stamp_ns_cache_ = sn;
  } else {
    sf = stamp_frames_cache_;
    sn = stamp_ns_cache_;
  }
  const double since = double(now_ns - sn) * 1e-9;
  const bool producer_live = since <= cfg_.max_extrapolate_s;
  const double ext = std::max(-cfg_.max_extrapolate_s, std::min(cfg_.max_extrapolate_s, since)) * cfg_.in_rate;
  const double level = double(sf) + ext - double(r_.load(std::memory_order_relaxed)) + rs_.unread();
  if (fresh_) {
    fresh_ = false;
    level_f_ = level;
    setpoint_ = level;
  } else if (producer_live) {
    level_f_ += (level - level_f_) * std::min(1.0, dt / cfg_.filter_s);
    const double err = (level_f_ - setpoint_) / cfg_.in_rate;  // seconds
    const double lim = cfg_.max_adjust / ki_;
    integ_ = std::max(-lim, std::min(lim, integ_ + err * dt));
    const double adj = std::max(-cfg_.max_adjust, std::min(cfg_.max_adjust, kp_ * err + ki_ * integ_));
    ratio_ = nominal_ * (1 + adj);
    adjust_ppm_.store(adj * 1e6, std::memory_order_relaxed);
  }
  level_ms_.store(level_f_ * 1000.0 / cfg_.in_rate, std::memory_order_relaxed);
  return true;
}

}  // namespace virgil
