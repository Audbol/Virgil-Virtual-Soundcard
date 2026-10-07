// Float <-> wire formats (big-endian L16 / L24) and driver int formats.
#pragma once

#include <cstdint>

namespace virgil {

inline int32_t float_to_int(float x, int32_t full_scale) {
  // Clamp then round; full_scale is 2^(bits-1).
  float s = x * float(full_scale);
  if (s >= float(full_scale - 1)) return full_scale - 1;
  if (s <= -float(full_scale)) return -full_scale;
  return int32_t(s < 0 ? s - 0.5f : s + 0.5f);
}

inline void float_to_l24(float x, uint8_t* out) {
  int32_t v = float_to_int(x, 1 << 23);
  out[0] = uint8_t(v >> 16);
  out[1] = uint8_t(v >> 8);
  out[2] = uint8_t(v);
}

inline float l24_to_float(const uint8_t* in) {
  int32_t v = (int32_t(in[0]) << 24 | int32_t(in[1]) << 16 | int32_t(in[2]) << 8) >> 8;
  return float(v) * (1.0f / 8388608.0f);
}

inline void float_to_l16(float x, uint8_t* out) {
  int32_t v = float_to_int(x, 1 << 15);
  out[0] = uint8_t(v >> 8);
  out[1] = uint8_t(v);
}

inline float l16_to_float(const uint8_t* in) {
  int16_t v = int16_t(uint16_t(in[0]) << 8 | in[1]);
  return float(v) * (1.0f / 32768.0f);
}

// Driver-side helpers (host-endian integers).
inline int32_t float_to_s32(float x) {
  double s = double(x) * 2147483648.0;
  if (s >= 2147483647.0) return 2147483647;
  if (s <= -2147483648.0) return int32_t(-2147483647 - 1);
  return int32_t(s);
}
inline float s32_to_float(int32_t v) { return float(double(v) * (1.0 / 2147483648.0)); }
inline int16_t float_to_s16(float x) { return int16_t(float_to_int(x, 1 << 15)); }
inline float s16_to_float(int16_t v) { return float(v) * (1.0f / 32768.0f); }

}  // namespace virgil
