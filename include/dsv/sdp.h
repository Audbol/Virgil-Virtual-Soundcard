// AES67 / SMPTE ST 2110-30 flavoured SDP (RFC 4566) build & parse.
#pragma once

#include <cstdint>
#include <string>

namespace dsv {

struct SdpInfo {
  std::string session_name;
  std::string session_info;  // i=
  uint64_t session_id = 0;
  uint64_t session_version = 0;
  std::string origin_address;      // o= unicast address of the sender
  std::string connection_address;  // c= (multicast group or unicast)
  uint32_t ttl = 32;
  uint16_t port = 5004;
  uint8_t payload_type = 96;
  std::string encoding = "L24";  // L16 | L24
  uint32_t sample_rate = 48000;
  uint32_t channels = 2;
  uint32_t ptime_us = 1000;
  uint32_t ts_offset = 0;           // a=mediaclk:direct=<offset>
  std::string ptp_grandmaster;      // XX-XX-XX-XX-XX-XX-XX-XX
  int ptp_domain = 0;
  std::string source_filter;        // a=source-filter incl address, if any

  uint32_t bytes_per_sample() const { return encoding == "L16" ? 2 : 3; }
  uint32_t frames_per_packet() const {
    return uint32_t((uint64_t(sample_rate) * ptime_us + 500000) / 1000000);
  }
};

std::string build_sdp(const SdpInfo& s);
bool parse_sdp(const std::string& text, SdpInfo* out);

}  // namespace dsv
