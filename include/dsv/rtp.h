// RFC 3550 RTP header handling (no CSRCs on transmit).
#pragma once

#include <cstddef>
#include <cstdint>

namespace dsv {

constexpr size_t kRtpHeaderBytes = 12;

struct RtpHeader {
  uint8_t payload_type = 96;
  bool marker = false;
  uint16_t sequence = 0;
  uint32_t timestamp = 0;
  uint32_t ssrc = 0;
};

inline size_t write_rtp_header(uint8_t* p, const RtpHeader& h) {
  p[0] = 0x80;  // V=2, no padding, no extension, CC=0
  p[1] = uint8_t((h.marker ? 0x80 : 0) | (h.payload_type & 0x7f));
  p[2] = uint8_t(h.sequence >> 8);
  p[3] = uint8_t(h.sequence);
  p[4] = uint8_t(h.timestamp >> 24);
  p[5] = uint8_t(h.timestamp >> 16);
  p[6] = uint8_t(h.timestamp >> 8);
  p[7] = uint8_t(h.timestamp);
  p[8] = uint8_t(h.ssrc >> 24);
  p[9] = uint8_t(h.ssrc >> 16);
  p[10] = uint8_t(h.ssrc >> 8);
  p[11] = uint8_t(h.ssrc);
  return kRtpHeaderBytes;
}

// Parses header, skipping CSRCs, header extension and padding.
inline bool parse_rtp(const uint8_t* p, size_t len, RtpHeader* h, const uint8_t** payload,
                      size_t* payload_len) {
  if (len < kRtpHeaderBytes || (p[0] >> 6) != 2) return false;
  const size_t cc = p[0] & 0x0f;
  size_t off = kRtpHeaderBytes + cc * 4;
  if (off > len) return false;
  if (p[0] & 0x10) {  // extension
    if (off + 4 > len) return false;
    const size_t ext_words = size_t(p[off + 2]) << 8 | p[off + 3];
    off += 4 + ext_words * 4;
    if (off > len) return false;
  }
  size_t end = len;
  if (p[0] & 0x20) {  // padding
    const size_t pad = p[len - 1];
    if (pad == 0 || pad > end - off) return false;
    end -= pad;
  }
  h->marker = (p[1] & 0x80) != 0;
  h->payload_type = p[1] & 0x7f;
  h->sequence = uint16_t(p[2] << 8 | p[3]);
  h->timestamp = uint32_t(p[4]) << 24 | uint32_t(p[5]) << 16 | uint32_t(p[6]) << 8 | p[7];
  h->ssrc = uint32_t(p[8]) << 24 | uint32_t(p[9]) << 16 | uint32_t(p[10]) << 8 | p[11];
  *payload = p + off;
  *payload_len = end - off;
  return true;
}

// Unwrap a 32-bit RTP timestamp to the 64-bit media frame nearest `ref`.
inline uint64_t unwrap_timestamp(uint32_t ts, uint64_t ref) {
  return ref + uint64_t(int64_t(int32_t(ts - uint32_t(ref))));
}

}  // namespace dsv
