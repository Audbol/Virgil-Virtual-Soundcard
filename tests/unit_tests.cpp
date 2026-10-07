#include <cmath>
#include <cstring>
#include <random>

#include "dsv/client.h"
#include "dsv/config.h"
#include "dsv/media_clock.h"
#include "dsv/ptp.h"
#include "dsv/rtp.h"
#include "dsv/sample_convert.h"
#include "dsv/sap.h"
#include "dsv/sdp.h"
#include "dsv/shm_layout.h"
#include "test_main.h"

using namespace dsv;

TEST(l24_roundtrip) {
  const float values[] = {0.f, 0.5f, -0.5f, 0.999f, -1.f, 1.0f, 2.0f, -3.0f, 1e-6f};
  for (float v : values) {
    uint8_t b[3];
    float_to_l24(v, b);
    const float clamped = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
    CHECK_NEAR(l24_to_float(b), clamped, 1.0 / 8388608.0 * 1.5);
  }
  uint8_t b[3];
  float_to_l24(-1.f, b);
  CHECK(b[0] == 0x80 && b[1] == 0 && b[2] == 0);
  float_to_l24(0.5f, b);
  CHECK(b[0] == 0x40 && b[1] == 0 && b[2] == 0);
}

TEST(l16_roundtrip) {
  for (float v : {0.f, 0.25f, -0.25f, -1.f}) {
    uint8_t b[2];
    float_to_l16(v, b);
    CHECK_NEAR(l16_to_float(b), v, 1.0 / 32768.0);
  }
}

TEST(rtp_header) {
  uint8_t buf[64] = {};
  RtpHeader h;
  h.payload_type = 97;
  h.sequence = 0xBEEF;
  h.timestamp = 0xDEADBEEF;
  h.ssrc = 0x12345678;
  h.marker = true;
  CHECK(write_rtp_header(buf, h) == 12);
  RtpHeader p;
  const uint8_t* pl;
  size_t plen;
  CHECK(parse_rtp(buf, 40, &p, &pl, &plen));
  CHECK(p.payload_type == 97 && p.sequence == 0xBEEF && p.timestamp == 0xDEADBEEF);
  CHECK(p.ssrc == 0x12345678 && p.marker);
  CHECK(pl == buf + 12 && plen == 28);

  // With one CSRC and an extension header.
  buf[0] = 0x80 | 0x10 | 0x01;
  buf[16] = 0xBE; buf[17] = 0xDE; buf[18] = 0; buf[19] = 1;  // 1 word
  CHECK(parse_rtp(buf, 40, &p, &pl, &plen));
  CHECK(pl == buf + 12 + 4 + 4 + 4);
  CHECK(!parse_rtp(buf, 10, &p, &pl, &plen));
}

TEST(timestamp_unwrap) {
  const uint64_t ref = 0x100000010ULL;
  CHECK(unwrap_timestamp(0x00000020u, ref) == 0x100000020ULL);
  CHECK(unwrap_timestamp(0xFFFFFFF0u, ref) == 0x0FFFFFFF0ULL);
  CHECK(unwrap_timestamp(0x00000000u, ref) == 0x100000000ULL);
}

TEST(media_clock_conversion) {
  const uint32_t rate = 48000;
  CHECK(ptp_ns_to_frames(1000000000LL, rate) == 48000);
  CHECK(ptp_ns_to_frames(1000000LL, rate) == 48);
  for (uint64_t f : {0ULL, 1ULL, 47ULL, 48000ULL, 81234567891234ULL}) {
    const int64_t ns = frames_to_ptp_ns(f, rate);
    CHECK(ptp_ns_to_frames(ns, rate) == f);
    CHECK(ptp_ns_to_frames(ns - 1, rate) + (f ? 1 : 0) == f);
  }
  ClockModel m{1000, 5000000, 1.0001};
  CHECK_NEAR(m.local_at(m.ptp_at(123456789)), 123456789, 2);
}

TEST(shm_anchor_seqlock) {
  alignas(64) static uint8_t mem[kShmHeaderBytes] = {};
  auto* h = reinterpret_cast<ShmHeader*>(mem);
  ClockAnchor a{1000, 48000, 48000e-9};
  publish_anchor(h, a);
  ClockAnchor b;
  CHECK(read_anchor(h, &b));
  CHECK(b.host_ns == 1000 && b.frame == 48000);
  CHECK_NEAR(b.frame_at(1000 + 1000000), 48048, 1e-6);
  CHECK_NEAR(b.host_ns_at(48048), 1000 + 1000000, 1);
}

static const char* kDanteSdp =
    "v=0\r\n"
    "o=- 1311738121 1311738121 IN IP4 192.168.1.71\r\n"
    "s=Y001-Yamaha-Ri8-D-14c7cc : 32\r\n"
    "c=IN IP4 239.69.161.58/32\r\n"
    "t=0 0\r\n"
    "a=keywds:Dante\r\n"
    "m=audio 5004 RTP/AVP 97\r\n"
    "i=2 channels: 01, 02\r\n"
    "a=recvonly\r\n"
    "a=rtpmap:97 L24/48000/2\r\n"
    "a=ptime:1\r\n"
    "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-14-C7-CC:0\r\n"
    "a=mediaclk:direct=750129\r\n";

TEST(sdp_parse_dante) {
  SdpInfo s;
  CHECK(parse_sdp(kDanteSdp, &s));
  CHECK(s.session_name == "Y001-Yamaha-Ri8-D-14c7cc : 32");
  CHECK(s.connection_address == "239.69.161.58");
  CHECK(s.port == 5004 && s.payload_type == 97);
  CHECK(s.encoding == "L24" && s.sample_rate == 48000 && s.channels == 2);
  CHECK(s.ptime_us == 1000 && s.ts_offset == 750129);
  CHECK(s.ptp_grandmaster == "00-1D-C1-FF-FE-14-C7-CC" && s.ptp_domain == 0);
  CHECK(s.frames_per_packet() == 48);
}

TEST(sdp_roundtrip) {
  SdpInfo a;
  a.session_name = "DSV 1-8";
  a.session_id = 42;
  a.origin_address = "10.0.0.5";
  a.connection_address = "239.69.83.67";
  a.channels = 8;
  a.payload_type = 98;
  a.ptime_us = 250;
  a.ptp_grandmaster = "00-11-22-FF-FE-33-44-55";
  SdpInfo b;
  const std::string text = build_sdp(a);
  CHECK(text.find("a=ptime:0.25\r\n") != std::string::npos);
  CHECK(text.find("a=framecount:12\r\n") != std::string::npos);
  CHECK(parse_sdp(text, &b));
  CHECK(b.session_name == a.session_name && b.channels == 8 && b.payload_type == 98);
  CHECK(b.ptime_us == 250 && b.connection_address == a.connection_address);
  CHECK(b.ptp_grandmaster == a.ptp_grandmaster);
}

TEST(sap_roundtrip) {
  auto pkt = sap::build_packet(kDanteSdp, 0xC0A80147, 0x1234, false);
  std::string sdp;
  bool del;
  uint16_t hash;
  uint32_t origin;
  CHECK(sap::parse_packet(pkt.data(), pkt.size(), &sdp, &del, &hash, &origin));
  CHECK(sdp == kDanteSdp && !del && hash == 0x1234 && origin == 0xC0A80147);
  auto d = sap::build_packet("v=0\r\n", 1, 2, true);
  CHECK(sap::parse_packet(d.data(), d.size(), &sdp, &del, &hash, &origin) && del);
  // No payload-type field.
  std::vector<uint8_t> raw = {0x20, 0, 0, 1, 10, 0, 0, 1, 'v', '=', '0'};
  CHECK(sap::parse_packet(raw.data(), raw.size(), &sdp, &del, &hash, &origin) && sdp == "v=0");
}

TEST(ptp_messages) {
  uint8_t buf[64];
  ptp::Header h;
  h.type = ptp::kSync;
  h.length = 44;
  h.domain = 3;
  h.flags = 0x0200;
  h.correction = int64_t(1234) << 16;
  h.source.clock = {1, 2, 3, 4, 5, 6, 7, 8};
  h.source.port = 9;
  h.sequence = 777;
  h.log_interval = -3;
  ptp::write_header(buf, h);
  ptp::write_timestamp(buf + 34, ptp::Timestamp::from_ns(1700000000123456789LL));
  ptp::Header p;
  CHECK(ptp::parse_header(buf, 44, &p));
  CHECK(p.type == ptp::kSync && p.domain == 3 && p.two_step() && p.correction_ns() == 1234);
  CHECK(p.source == h.source && p.sequence == 777 && p.log_interval == -3);
  CHECK(ptp::read_timestamp(buf + 34).ns() == 1700000000123456789LL);

  ptp::Announce a, b;
  a.priority1 = 128;
  a.grandmaster = {1};
  b.priority1 = 250;
  b.grandmaster = {2};
  ptp::PortId pa, pb;
  CHECK(ptp::compare_announce(a, pa, b, pb) < 0);
  CHECK(ptp::compare_announce(b, pb, a, pa) > 0);
  ptp::write_announce(buf, h, a);
  ptp::Announce r;
  CHECK(ptp::parse_announce(buf, 64, &r));
  CHECK(r.priority1 == 128 && r.grandmaster == a.grandmaster);
  CHECK(ptp::format_clock_id(h.source.clock) == "01-02-03-04-05-06-07-08");
}

TEST(servo_converges) {
  // Local oscillator 80 ppm fast with 20 us timestamp noise; 8 Sync/s.
  std::mt19937 rng(1);
  std::normal_distribution<double> noise(0, 20000);
  PiServo servo;
  const double drift = 80e-6;
  const int64_t master0 = 1700000000000000000LL;
  int64_t worst_late = 0;
  for (int i = 0; i < 8 * 120; ++i) {
    const int64_t master = master0 + int64_t(i) * 125000000LL;
    const int64_t local = 5000000000LL + int64_t(double(master - master0) * (1.0 + drift));
    servo.sample(local + int64_t(noise(rng)), master);
    if (i > 8 * 90) {
      const int64_t err = servo.model().ptp_at(local) - master;
      worst_late = std::max<int64_t>(worst_late, std::llabs(err));
    }
  }
  CHECK(servo.locked());
  CHECK(worst_late < 30000);  // within 30 us of the master after 90 s
  CHECK_NEAR(servo.model().ratio, 1.0 / (1.0 + drift), 5e-6);
}

TEST(config_parse) {
  const char* text =
      "[device]\n"
      "name = Studio DSV\n"
      "interface = 127.0.0.1\n"
      "tx_channels = 16 ; comment\n"
      "rx_channels = 4\n"
      "packet_time_us = 250\n"
      "latency_us = 1000\n"
      "clock = free\n"
      "[tx]\n"
      "address = 239.69.1.1\n"
      "channels = 8\n"
      "[tx]\n"
      "address = 239.69.1.2\n"
      "first_channel = 9\n"
      "channels = 8\n"
      "[rx]\n"
      "sap_name = \"Ri8 : 32\"\n"
      "channels = 2\n";
  Config c;
  std::string err;
  CHECK(parse_config(text, &c, &err));
  CHECK(validate_config(&c, &err));
  CHECK(c.device_name == "Studio DSV" && c.tx.size() == 2 && c.rx.size() == 1);
  CHECK(c.tx[1].first_channel == 9 && c.rx[0].sap_name == "Ri8 : 32");
  CHECK(c.period_frames() == 12 && c.rx_latency_frames() == 48);
  CHECK((c.ring_frames & (c.ring_frames - 1)) == 0 && c.ring_frames >= 4096);
  CHECK(c.tx[0].name == "Studio DSV 1-8");

  Config bad;
  CHECK(!parse_config("[device]\nbogus = 1\n", &bad, &err));
  CHECK(err.find("line 2") != std::string::npos);
  Config mtu;
  parse_config("[device]\ntx_channels=64\n[tx]\naddress=239.1.1.1\nchannels=64\n", &mtu, &err);
  CHECK(!validate_config(&mtu, &err));  // 64ch L24 @1ms > MTU
}

TEST(config_format_roundtrip) {
  Config a;
  std::string err;
  parse_config("[device]\nname = Studio ; A\ninterface = 10.0.0.2\ntx_channels = 16\n"
               "control_port = 0\n[tx]\naddress=239.1.1.1\nchannels=8\n"
               "[rx]\nsap_name = \"Ri8 : 32 # x\"\nchannels = 2\nfirst_channel = 3\n",
               &a, &err);
  CHECK(a.device_name == "Studio");
  Config b;
  CHECK(parse_config(format_config(a), &b, &err));
  CHECK(b.device_name == a.device_name && b.interface == "10.0.0.2");
  CHECK(b.tx_channels == 16 && b.control_port == 0);
  CHECK(b.tx.size() == 1 && b.tx[0].address == "239.1.1.1" && b.tx[0].channels == 8);
  CHECK(b.rx.size() == 1 && b.rx[0].sap_name == "Ri8 : 32 # x" && b.rx[0].first_channel == 3);
  CHECK(format_config(a) == format_config(b));
}

TEST_MAIN()
