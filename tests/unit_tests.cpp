#include <cmath>
#include <cstring>
#include <random>

#include "virgil/client.h"
#include "virgil/config.h"
#include "virgil/media_clock.h"
#include "virgil/platform.h"
#include "virgil/ptp.h"
#include "virgil/sample_convert.h"
#include "virgil/shm_layout.h"
#include "test_main.h"

using namespace virgil;

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

TEST(ptp1_messages) {
  uint8_t buf[128];
  ptp1::Header h;
  h.message_type = ptp1::kEventMessage;
  h.control = ptp1::kSync;
  h.source.uuid = {0x00, 0x1d, 0xc1, 0x12, 0x34, 0x56};
  h.source.port = 1;
  h.sequence = 4711;
  h.flags = ptp1::kFlagAssist;
  ptp1::ClockProps props;
  props.gm_uuid = h.source.uuid;
  props.stratum = 3;
  props.preferred = true;
  props.variance = -4000;
  props.sync_interval = -2;
  CHECK(ptp1::write_sync(buf, h, ptp1::Timestamp::from_ns(1234567890123456789LL % 4000000000000000000LL),
                         props, h.source) == 124);
  // Wire layout as Dante sends it.
  CHECK(buf[0] == 0 && buf[1] == 1 && std::memcmp(buf + 4, "_DFLT", 5) == 0);
  CHECK(buf[20] == 1 && buf[32] == 0 && (buf[35] & 0x08));
  ptp1::Header p;
  CHECK(ptp1::parse_header(buf, sizeof buf, &p));
  CHECK(p.source == h.source && p.sequence == 4711 && p.control == ptp1::kSync);
  ptp1::Timestamp ts;
  ptp1::ClockProps pr;
  CHECK(ptp1::parse_sync(buf, 124, &ts, &pr));
  CHECK(pr.stratum == 3 && pr.preferred && pr.variance == -4000 && pr.sync_interval == -2);
  CHECK(pr.gm_uuid == props.gm_uuid);
  CHECK(!ptp1::parse_sync(buf, 100, &ts, &pr));

  ptp1::write_follow_up(buf, h, 4711, ptp1::Timestamp{1700000000, 999999999});
  uint16_t seq;
  CHECK(ptp1::parse_follow_up(buf, 52, &seq, &ts));
  CHECK(seq == 4711 && ts.seconds == 1700000000 && ts.nanoseconds == 999999999);
  CHECK(buf[42] == 0x12 && buf[43] == 0x67);  // associatedSequenceId at 42 (spec layout)

  ptp1::PortId req{{2, 3, 4, 5, 6, 7}, 1};
  ptp1::write_delay_resp(buf, h, ptp1::Timestamp{5, 6}, req, 99);
  ptp1::PortId rq;
  uint16_t rs;
  CHECK(ptp1::parse_delay_resp(buf, 60, &ts, &rq, &rs));
  CHECK(rq == req && rs == 99 && ts.seconds == 5 && ts.nanoseconds == 6);

  // PTPv2 packets are not mistaken for v1.
  uint8_t v2[44] = {0x00, 0x02};
  CHECK(!ptp1::parse_header(v2, sizeof v2, &p));

  // Master choice: preferred, then stratum, variance, uuid.
  ptp1::ClockProps a, b;
  ptp1::PortId pa{{1}, 1}, pb{{2}, 1};
  a.stratum = 4; b.stratum = 3;
  CHECK(ptp1::compare_masters(b, pb, a, pa) < 0);
  a.preferred = true;
  CHECK(ptp1::compare_masters(a, pa, b, pb) < 0);
  CHECK(ptp1::format_uuid(h.source.uuid) == "00:1d:c1:12:34:56");
}

TEST(ptp1_master_follower_lock) {
  // Two PTPv1 clocks on this host: one becomes master, the other must follow
  // it to within a few hundred microseconds (software timestamps).
  uint32_t lo = 0x7F000001;
  PtpClock master, follower;
  PtpClock::Options mo;
  mo.interface_addr = lo;
  mo.master_capable = true;
  PtpClock::Options fo;
  fo.interface_addr = lo;
  if (!master.start(mo) || !follower.start(fo)) {
    std::printf("  (skipped: cannot bind PTP ports 319/320 here)\n");
    return;
  }
  const int64_t deadline = mono_ns() + 25000000000LL;
  bool locked = false;
  while (mono_ns() < deadline && !locked) {
    sleep_until_ns(mono_ns() + 200000000LL);
    locked = master.state() == kStatePtpMaster && follower.state() == kStatePtpLocked;
  }
  if (!locked) {
    // multicast loopback may be unavailable (some containers)
    std::printf("  (skipped: no multicast loopback; master=%u follower=%u)\n",
                unsigned(master.state()), unsigned(follower.state()));
    return;
  }
  const int64_t t = mono_ns();
  const int64_t diff = master.model().ptp_at(t) - follower.model().ptp_at(t);
  std::printf("  follower offset from master: %.1f us, gm %s\n", diff / 1000.0,
              follower.grandmaster().c_str());
  CHECK(std::llabs(diff) < 500000);
  CHECK(follower.grandmaster() == master.grandmaster());
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

TEST(servo_after_reset_to) {
  // As PtpClock does: start from a realtime-based model, then follow a master
  // whose time is close to it. The frequency estimate must be measured from
  // the first real sample, not from the reset model.
  PiServo servo;
  ClockModel start;
  start.base_local = 5000000000LL;
  start.base_ptp = 1700000000000000000LL;
  servo.reset_to(start);
  const int64_t offset = 30000;  // master 30 us ahead of our start model
  for (int i = 0; i < 4 * 60; ++i) {
    const int64_t local = 6000000000LL + int64_t(i) * 250000000LL;
    servo.sample(local, start.ptp_at(local) + offset);
  }
  CHECK(servo.locked());
  CHECK_NEAR(servo.model().ratio, 1.0, 2e-6);
}

TEST(servo_ignores_single_late_timestamp) {
  // A locked servo sees one sample 1.5 ms late (a descheduled receive thread):
  // it must not step the clock, and must still step for a real, lasting jump.
  PiServo servo;
  int64_t local = 1000000000LL, master = 1700000000000000000LL;
  for (int i = 0; i < 80; ++i, local += 250000000LL, master += 250000000LL) servo.sample(local, master);
  CHECK(servo.locked());
  CHECK(servo.sample(local + 1500000, master) == PiServo::kOutlier);
  local += 250000000LL, master += 250000000LL;
  CHECK(servo.sample(local, master) != PiServo::kStep);
  for (int i = 0; i < 3; ++i) {
    local += 250000000LL, master += 250000000LL;
    const auto r = servo.sample(local, master + 5000000);
    if (i < 2) CHECK(r == PiServo::kOutlier);
    else CHECK(r == PiServo::kStep);
  }
}

TEST(config_parse) {
  const char* text =
      "[device]\n"
      "name = Studio Rack\n"
      "interface = 127.0.0.1\n"
      "tx_channels = 16 ; comment\n"
      "rx_channels = 4\n"
      "latency_us = 2000\n"
      "tx_latency_us = 4000\n"
      "clock = free\n"
      "[ptp]\n"
      "subdomain = _DFLT\n"
      "master_capable = true\n";
  Config c;
  std::string err;
  CHECK(parse_config(text, &c, &err));
  CHECK(validate_config(&c, &err));
  CHECK(c.device_name == "Studio Rack" && c.tx_channels == 16 && c.rx_channels == 4);
  CHECK(c.latency_us == 2000 && c.tx_latency_us == 4000 && c.ptp_master_capable);
  CHECK(c.period_frames() == 48 && c.rx_latency_frames() == 144 && c.tx_lead_frames() == 96);
  CHECK((c.ring_frames & (c.ring_frames - 1)) == 0 && c.ring_frames >= 8192);

  Config bad;
  CHECK(!parse_config("[device]\nbogus = 1\n", &bad, &err));
  CHECK(err.find("line 2") != std::string::npos);
  Config name;
  parse_config("[device]\nname = bad/name\n", &name, &err);
  CHECK(!validate_config(&name, &err));
  Config lat;
  parse_config("[device]\ntx_latency_us = 1000\ntick_us = 1000\n", &lat, &err);
  CHECK(!validate_config(&lat, &err));  // below two ticks
}

TEST(config_legacy_aes67_is_ignored) {
  // A config from the AES67 version still loads; flows are reported, not fatal.
  const char* old =
      "[device]\nname = Virgil\npacket_time_us = 1000\nsap = true\n"
      "[ptp]\ndomain = 0\n"
      "[tx]\naddress = 239.69.1.1\nchannels = 8\n"
      "[rx]\nsap_name = \"Ri8 : 32\"\n";
  Config c;
  std::string err;
  CHECK(parse_config(old, &c, &err));
  CHECK(validate_config(&c, &err));
  CHECK(c.device_name == "Virgil" && c.notes.size() >= 3);
}

TEST(config_format_roundtrip) {
  Config a;
  std::string err;
  parse_config("[device]\nname = Studio ; A\ninterface = 10.0.0.2\ntx_channels = 16\n"
               "control_port = 0\nlatency_us = 1500\n[ptp]\nsubdomain = _DFLT\n",
               &a, &err);
  CHECK(a.device_name == "Studio");
  Config b;
  CHECK(parse_config(format_config(a), &b, &err));
  CHECK(b.device_name == a.device_name && b.interface == "10.0.0.2");
  CHECK(b.tx_channels == 16 && b.control_port == 0 && b.latency_us == 1500);
  CHECK(format_config(a) == format_config(b));
}

TEST_MAIN()
