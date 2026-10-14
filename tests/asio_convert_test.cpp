// Audio interface sample conversion and channel selection
// (tools/control_win/asio_convert.h, used by Virgil Control's ASIO host and
// WASAPI multichannel paths). Part of virgil_unit_tests: portable.
#include <cstdint>
#include <cstring>
#include <vector>

#include "../tools/control_win/asio_convert.h"
#include "test_main.h"

using namespace asioconv;

namespace {

const long kAllTypes[] = {kInt16MSB,   kInt24MSB,   kInt32MSB,   kFloat32MSB, kFloat64MSB, kInt32MSB16,
                          kInt32MSB18, kInt32MSB20, kInt32MSB24, kInt16LSB,   kInt24LSB,   kInt32LSB,
                          kFloat32LSB, kFloat64LSB, kInt32LSB16, kInt32LSB18, kInt32LSB20, kInt32LSB24};

double step_of(long t) {
  switch (t) {
    case kInt16MSB: case kInt16LSB: case kInt32MSB16: case kInt32LSB16: return 1.0 / 32768;
    case kInt32MSB18: case kInt32LSB18: return 1.0 / 131072;
    case kInt32MSB20: case kInt32LSB20: return 1.0 / 524288;
    case kInt24MSB: case kInt24LSB: case kInt32MSB24: case kInt32LSB24: return 1.0 / 8388608;
    case kInt32MSB: case kInt32LSB: return 1.0 / 2147483648.0;
    default: return 1e-7;  // float
  }
}

}  // namespace

TEST(asio_sample_sizes) {
  CHECK(sample_bytes(kInt16LSB) == 2);
  CHECK(sample_bytes(kInt24LSB) == 3);
  CHECK(sample_bytes(kInt24MSB) == 3);
  CHECK(sample_bytes(kInt32LSB) == 4);
  CHECK(sample_bytes(kInt32LSB24) == 4);
  CHECK(sample_bytes(kFloat32LSB) == 4);
  CHECK(sample_bytes(kFloat64LSB) == 8);
  CHECK(sample_bytes(kFloat64MSB) == 8);
  CHECK(!supported(32));  // DSD
  CHECK(!supported(-1));
}

TEST(asio_known_bit_patterns) {
  float f[2];
  // Int16LSB: 0x4000 = +0.5, 0x8000 = -1.
  const uint8_t i16[] = {0x00, 0x40, 0x00, 0x80};
  CHECK(to_float(kInt16LSB, i16, 2, 2, f, 1));
  CHECK_NEAR(f[0], 0.5, 1e-9);
  CHECK_NEAR(f[1], -1.0, 1e-9);
  // Int16MSB, same values big-endian.
  const uint8_t i16be[] = {0x40, 0x00, 0x80, 0x00};
  CHECK(to_float(kInt16MSB, i16be, 2, 2, f, 1));
  CHECK_NEAR(f[0], 0.5, 1e-9);
  CHECK_NEAR(f[1], -1.0, 1e-9);
  // Int24LSB packed: 0x400000 = +0.5, 0xC00000 = -0.5.
  const uint8_t i24[] = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0};
  CHECK(to_float(kInt24LSB, i24, 3, 2, f, 1));
  CHECK_NEAR(f[0], 0.5, 1e-9);
  CHECK_NEAR(f[1], -0.5, 1e-9);
  // Int32LSB24: right-justified 24 bits, sign-extended (or not: upper byte ignored).
  const uint8_t r24[] = {0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0, 0x00};
  CHECK(to_float(kInt32LSB24, r24, 4, 2, f, 1));
  CHECK_NEAR(f[0], 0.5, 1e-9);
  CHECK_NEAR(f[1], -0.5, 1e-9);
  // Int32LSB16: 0x00004000 = +0.5.
  const uint8_t r16[] = {0x00, 0x40, 0x00, 0x00, 0x00, 0xC0, 0xFF, 0xFF};
  CHECK(to_float(kInt32LSB16, r16, 4, 2, f, 1));
  CHECK_NEAR(f[0], 0.5, 1e-9);
  CHECK_NEAR(f[1], -0.5, 1e-9);
  // Int32LSB: 0x40000000 = +0.5.
  const uint8_t i32[] = {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x80};
  CHECK(to_float(kInt32LSB, i32, 4, 2, f, 1));
  CHECK_NEAR(f[0], 0.5, 1e-9);
  CHECK_NEAR(f[1], -1.0, 1e-9);
  // Float32MSB: 0.25 = 0x3E800000.
  const uint8_t f32be[] = {0x3E, 0x80, 0x00, 0x00};
  CHECK(to_float(kFloat32MSB, f32be, 4, 1, f, 1));
  CHECK_NEAR(f[0], 0.25, 1e-9);
  // NaN is silence.
  const float nan = std::nanf("");
  CHECK(to_float(kFloat32LSB, &nan, 4, 1, f, 1));
  CHECK(f[0] == 0.f);
  // Unsupported: silence, false.
  f[0] = 1;
  CHECK(!to_float(99, i32, 4, 1, f, 1));
  CHECK(f[0] == 0.f);
}

TEST(asio_round_trip_every_type) {
  const float in[] = {0.f, 0.5f, -0.5f, 0.123456f, -0.987654f, 0.999f, -1.f, 0.25f};
  const size_t n = sizeof in / sizeof in[0];
  for (long t : kAllTypes) {
    const unsigned b = sample_bytes(t);
    CHECK(b != 0);
    std::vector<uint8_t> buf(n * b + 1, 0xEE);
    CHECK(from_float(t, in, 1, n, buf.data(), b));
    CHECK(buf[n * b] == 0xEE);  // nothing past the end
    float out[n];
    CHECK(to_float(t, buf.data(), b, n, out, 1));
    for (size_t i = 0; i < n; ++i) CHECK_NEAR(out[i], in[i], step_of(t));
  }
}

TEST(asio_clamps_and_rounds) {
  const float in[] = {1.5f, -1.5f, 1.f, std::nanf("")};
  for (long t : kAllTypes) {
    const unsigned b = sample_bytes(t);
    std::vector<uint8_t> buf(4 * b);
    from_float(t, in, 1, 4, buf.data(), b);
    float out[4];
    to_float(t, buf.data(), b, 4, out, 1);
    const bool is_float = t == kFloat32LSB || t == kFloat32MSB || t == kFloat64LSB || t == kFloat64MSB;
    if (is_float) {
      CHECK_NEAR(out[0], 1.5, 1e-6);  // float formats are not clipped
    } else {
      CHECK(out[0] <= 1.f);  // (2^31 - 1) / 2^31 is 1.0f in float
      CHECK_NEAR(out[0], 1.0, step_of(t) * 1.01);
      CHECK_NEAR(out[1], -1.0, 1e-9);
      CHECK_NEAR(out[2], 1.0, step_of(t) * 1.01);  // +1.0 is one step below full scale
    }
    CHECK(out[3] == 0.f);  // NaN
  }
  // Int24LSB of +1: 0x7FFFFF exactly.
  uint8_t p[3];
  const float one = 1.f;
  from_float(kInt24LSB, &one, 1, 1, p, 3);
  CHECK(p[0] == 0xFF && p[1] == 0xFF && p[2] == 0x7F);
  // Silence (src nullptr) is zeros.
  uint8_t z[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  from_float(kFloat64LSB, nullptr, 1, 1, z, 8);
  for (uint8_t v : z) CHECK(v == 0);
}

TEST(asio_strides_interleave_and_deinterleave) {
  // Two ASIO channel buffers (one Int32LSB, one Int16LSB) into interleaved
  // stereo, as the host's bufferSwitch does.
  const int32_t a[3] = {0x40000000, 0, -0x40000000};
  const int16_t b[3] = {0x2000, 0x4000, 0};
  float il[6];
  CHECK(to_float(kInt32LSB, a, 4, 3, il + 0, 2));
  CHECK(to_float(kInt16LSB, b, 2, 3, il + 1, 2));
  CHECK_NEAR(il[0], 0.5, 1e-9);
  CHECK_NEAR(il[1], 0.25, 1e-9);
  CHECK_NEAR(il[2], 0.0, 1e-9);
  CHECK_NEAR(il[3], 0.5, 1e-9);
  CHECK_NEAR(il[4], -0.5, 1e-9);
  CHECK_NEAR(il[5], 0.0, 1e-9);
  // Interleaved float -> the second channel of an interleaved 3-channel
  // 24-bit stream (WASAPI render), the others untouched.
  uint8_t dev[3 * 9];
  std::memset(dev, 0xAA, sizeof dev);
  CHECK(from_float(kInt24LSB, il + 0, 2, 3, dev + 3, 9));
  for (size_t f = 0; f < 3; ++f) {
    for (size_t k = 0; k < 3; ++k) CHECK(dev[f * 9 + k] == 0xAA);
    for (size_t k = 6; k < 9; ++k) CHECK(dev[f * 9 + k] == 0xAA);
  }
  float back[3];
  CHECK(to_float(kInt24LSB, dev + 3, 9, 3, back, 1));
  CHECK_NEAR(back[0], 0.5, 1e-6);
  CHECK_NEAR(back[2], -0.5, 1e-6);
}

TEST(asio_select_channels) {
  Range r = select_channels(1, 8, 18);
  CHECK(r.first0 == 0 && r.count == 8 && !r.clamped);
  r = select_channels(11, 8, 18);
  CHECK(r.first0 == 10 && r.count == 8 && !r.clamped);
  r = select_channels(15, 8, 18);  // 15..18: four of eight
  CHECK(r.first0 == 14 && r.count == 4 && r.clamped);
  r = select_channels(19, 2, 18);  // past the end
  CHECK(r.count == 0 && r.clamped);
  r = select_channels(0, 2, 2);  // 0 is taken as 1
  CHECK(r.first0 == 0 && r.count == 2 && !r.clamped);
  r = select_channels(1, 64, 64);
  CHECK(r.count == 64 && !r.clamped);
  r = select_channels(3, 2, 0);  // a device without outputs
  CHECK(r.count == 0 && r.clamped);
}

TEST(asio_buffer_sizes) {
  // Powers of two from 32 to 2048, preferred 256.
  CHECK(buffer_allowed(128, 32, 2048, 256, -1));
  CHECK(!buffer_allowed(96, 32, 2048, 256, -1));
  CHECK(!buffer_allowed(4096, 32, 2048, 256, -1));
  // Steps of 16 from 48.
  CHECK(buffer_allowed(96, 48, 1024, 480, 16));
  CHECK(!buffer_allowed(100, 48, 1024, 480, 16));
  // Only the preferred size.
  CHECK(buffer_allowed(512, 512, 512, 512, 0));
  CHECK(!buffer_allowed(256, 512, 512, 512, 0));
  CHECK(!buffer_allowed(0, 32, 2048, 256, -1));
}
