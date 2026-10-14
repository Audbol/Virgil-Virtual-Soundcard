// Sample conversion and channel selection for Virgil Control's audio
// interface sources and outputs (tools/control_win/asio_host.cpp hosts an
// interface's ASIO driver; wasapi.cpp uses the same conversion for
// multichannel capture / render).
//
// Portable, header only, no ASIO SDK needed: the sample type numbers are the
// ASIO 2 ASIOSampleType values (asio.h), spelled out so the unit tests build
// everywhere. No allocation, no locks: safe in an ASIO bufferSwitch.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace asioconv {

// ASIOSampleType (asio.h). MSB = big-endian; IntXXLSBnn = nn valid bits,
// right-justified in a 32-bit container.
enum SampleType : long {
  kInt16MSB = 0,
  kInt24MSB = 1,
  kInt32MSB = 2,
  kFloat32MSB = 3,
  kFloat64MSB = 4,
  kInt32MSB16 = 8,
  kInt32MSB18 = 9,
  kInt32MSB20 = 10,
  kInt32MSB24 = 11,
  kInt16LSB = 16,
  kInt24LSB = 17,
  kInt32LSB = 18,
  kFloat32LSB = 19,
  kFloat64LSB = 20,
  kInt32LSB16 = 24,
  kInt32LSB18 = 25,
  kInt32LSB20 = 26,
  kInt32LSB24 = 27,
};

// Bytes per sample; 0 = a type this code does not convert (DSD, unknown).
inline unsigned sample_bytes(long type) {
  switch (type) {
    case kInt16MSB: case kInt16LSB: return 2;
    case kInt24MSB: case kInt24LSB: return 3;
    case kInt32MSB: case kInt32LSB: case kFloat32MSB: case kFloat32LSB:
    case kInt32MSB16: case kInt32MSB18: case kInt32MSB20: case kInt32MSB24:
    case kInt32LSB16: case kInt32LSB18: case kInt32LSB20: case kInt32LSB24: return 4;
    case kFloat64MSB: case kFloat64LSB: return 8;
    default: return 0;
  }
}
inline bool supported(long type) { return sample_bytes(type) != 0; }
inline const char* type_name(long type) {
  switch (type) {
    case kInt16MSB: return "Int16MSB";
    case kInt24MSB: return "Int24MSB";
    case kInt32MSB: return "Int32MSB";
    case kFloat32MSB: return "Float32MSB";
    case kFloat64MSB: return "Float64MSB";
    case kInt32MSB16: return "Int32MSB16";
    case kInt32MSB18: return "Int32MSB18";
    case kInt32MSB20: return "Int32MSB20";
    case kInt32MSB24: return "Int32MSB24";
    case kInt16LSB: return "Int16LSB";
    case kInt24LSB: return "Int24LSB";
    case kInt32LSB: return "Int32LSB";
    case kFloat32LSB: return "Float32LSB";
    case kFloat64LSB: return "Float64LSB";
    case kInt32LSB16: return "Int32LSB16";
    case kInt32LSB18: return "Int32LSB18";
    case kInt32LSB20: return "Int32LSB20";
    case kInt32LSB24: return "Int32LSB24";
    default: return "unknown";
  }
}

namespace detail {
inline bool big_endian(long type) { return type >= 0 && type < 16; }
// Valid bits of an integer type (the full scale is 2^(bits-1)); 0 for float.
inline unsigned int_bits(long type) {
  switch (type) {
    case kInt16MSB: case kInt16LSB: case kInt32MSB16: case kInt32LSB16: return 16;
    case kInt32MSB18: case kInt32LSB18: return 18;
    case kInt32MSB20: case kInt32LSB20: return 20;
    case kInt24MSB: case kInt24LSB: case kInt32MSB24: case kInt32LSB24: return 24;
    case kInt32MSB: case kInt32LSB: return 32;
    default: return 0;
  }
}
// Little-endian bytes of one sample (n bytes) <-> the stored order.
inline void load(const uint8_t* p, unsigned n, bool be, uint8_t* le) {
  for (unsigned i = 0; i < n; ++i) le[i] = be ? p[n - 1 - i] : p[i];
}
inline void store(const uint8_t* le, unsigned n, bool be, uint8_t* p) {
  for (unsigned i = 0; i < n; ++i) p[i] = be ? le[n - 1 - i] : le[i];
}
inline float decode(const uint8_t* p, long type, unsigned bytes, unsigned bits, bool be) {
  uint8_t b[8];
  load(p, bytes, be, b);
  if (bits == 0) {
    if (bytes == 4) {
      float f;
      std::memcpy(&f, b, 4);
      return f == f ? f : 0.f;  // NaN: silence
    }
    double d;
    std::memcpy(&d, b, 8);
    return d == d ? float(d) : 0.f;
  }
  int32_t v;
  if (bytes == 2) {
    v = int16_t(uint16_t(b[0] | b[1] << 8));
  } else if (bytes == 3) {
    v = int32_t(uint32_t(b[0]) << 8 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 24) >> 8;
  } else {
    uint32_t u = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    if (bits < 32) u <<= 32 - bits;  // right-justified: sign-extend from the valid bits
    v = int32_t(u);
    if (bits < 32) v >>= 32 - bits;
  }
  (void)type;
  return float(double(v) / double(int64_t(1) << (bits - 1)));
}
inline void encode(float x, uint8_t* p, unsigned bytes, unsigned bits, bool be) {
  uint8_t b[8];
  if (!(x == x)) x = 0.f;
  if (bits == 0) {
    if (bytes == 4) std::memcpy(b, &x, 4);
    else {
      const double d = x;
      std::memcpy(b, &d, 8);
    }
    store(b, bytes, be, p);
    return;
  }
  const double full = double(int64_t(1) << (bits - 1));
  double s = std::floor(double(x) * full + 0.5);
  if (s > full - 1) s = full - 1;
  if (s < -full) s = -full;
  const int32_t v = int32_t(s);
  const uint32_t u = uint32_t(v);
  b[0] = uint8_t(u);
  b[1] = uint8_t(u >> 8);
  b[2] = uint8_t(u >> 16);
  b[3] = uint8_t(u >> 24);  // right-justified types keep the sign extension
  store(b, bytes, be, p);
}
}  // namespace detail

// `frames` samples of one channel -> float. src: the first sample;
// src_stride: bytes from one sample to the next (sample_bytes(type) for an
// ASIO buffer, the frame size for interleaved data). dst: dst_stride floats
// apart (the channel count for interleaved output). Unsupported types give
// silence. Returns false for them.
inline bool to_float(long type, const void* src, size_t src_stride, size_t frames, float* dst, size_t dst_stride) {
  const unsigned bytes = sample_bytes(type);
  if (!bytes || !src) {
    for (size_t i = 0; i < frames; ++i) dst[i * dst_stride] = 0.f;
    return src != nullptr && bytes != 0;
  }
  const unsigned bits = detail::int_bits(type);
  const bool be = detail::big_endian(type);
  const uint8_t* p = static_cast<const uint8_t*>(src);
  if (type == kFloat32LSB) {  // the usual cases, without the byte shuffling
    for (size_t i = 0; i < frames; ++i) {
      float f;
      std::memcpy(&f, p + i * src_stride, 4);
      dst[i * dst_stride] = f == f ? f : 0.f;
    }
    return true;
  }
  if (type == kInt32LSB) {
    for (size_t i = 0; i < frames; ++i) {
      int32_t v;
      std::memcpy(&v, p + i * src_stride, 4);
      dst[i * dst_stride] = float(v) * (1.f / 2147483648.f);
    }
    return true;
  }
  for (size_t i = 0; i < frames; ++i) dst[i * dst_stride] = detail::decode(p + i * src_stride, type, bytes, bits, be);
  return true;
}

// float -> `frames` samples of one channel (clamped to full scale, rounded).
// src: src_stride floats apart, nullptr = silence; dst: dst_stride bytes apart.
inline bool from_float(long type, const float* src, size_t src_stride, size_t frames, void* dst, size_t dst_stride) {
  const unsigned bytes = sample_bytes(type);
  if (!bytes || !dst) return false;
  const unsigned bits = detail::int_bits(type);
  const bool be = detail::big_endian(type);
  uint8_t* p = static_cast<uint8_t*>(dst);
  for (size_t i = 0; i < frames; ++i) {
    if (!src) std::memset(p + i * dst_stride, 0, bytes);
    else detail::encode(src[i * src_stride], p + i * dst_stride, bytes, bits, be);
  }
  return true;
}

// Which of a device's channels a source / output uses: `count` channels from
// `first` (1-based, as in the settings), within the `available` the device
// has. first0 is 0-based; count is 0 when first is past the last channel.
struct Range {
  unsigned first0 = 0, count = 0;
  bool clamped = false;  // fewer than asked for (or none)
};
inline Range select_channels(unsigned first, unsigned count, unsigned available) {
  Range r;
  if (first < 1) first = 1;
  r.first0 = first - 1;
  if (count == 0 || r.first0 >= available) {
    r.count = 0;
    r.clamped = count != 0;
    return r;
  }
  r.count = count;
  if (r.first0 + count > available) {
    r.count = available - r.first0;
    r.clamped = true;
  }
  return r;
}

// Is `size` a buffer size the driver allows (ASIOGetBufferSize: min, max,
// preferred, granularity; granularity -1 = powers of two, 0 = only the
// preferred size)?
inline bool buffer_allowed(long size, long min, long max, long pref, long gran) {
  if (size <= 0) return false;
  if (size == pref) return true;
  if (size < min || size > max) return false;
  if (gran == -1) return (size & (size - 1)) == 0;
  if (gran <= 0) return min != max ? false : size == min;
  return (size - min) % gran == 0;
}

}  // namespace asioconv
