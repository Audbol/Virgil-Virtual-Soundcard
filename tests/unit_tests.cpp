#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>

#include "virgil/client.h"
#include "virgil/config.h"
#include "virgil/engine.h"
#include "virgil/media_clock.h"
#include "virgil/platform.h"
#include "virgil/ptp.h"
#include "virgil/resampler.h"
#include "virgil/sample_convert.h"
#include "virgil/shm_layout.h"
#include "config_model.h"
#include "desktop_audio_settings.h"
#include "diagnostics.h"
#include "diagnostics_model.h"
#include "remote_auth.h"
#include "settings_model.h"
#include "test_main.h"
#include "view.h"

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

TEST(s32_conversions) {
  // Clamping, truncation and NaN of the driver / Dante ring format.
  CHECK(float_to_s32(0.f) == 0);
  CHECK(float_to_s32(0.5f) == (1 << 30));
  CHECK(float_to_s32(-1.f) == int32_t(-2147483647 - 1));
  CHECK(float_to_s32(1.f) == 2147483647);
  CHECK(float_to_s32(3.f) == 2147483647 && float_to_s32(-3.f) == int32_t(-2147483647 - 1));
  CHECK(float_to_s32(INFINITY) == 2147483647 && float_to_s32(-INFINITY) == int32_t(-2147483647 - 1));
  CHECK(float_to_s32(NAN) == 0);
  CHECK(float_to_s32(std::nextafter(1.f, 0.f)) == 2147483520);  // 2^31 - 128: largest below full scale
  CHECK(float_to_s32(-1e-9f) == -2);                             // truncates toward zero
  CHECK(s32_to_float(int32_t(-2147483647 - 1)) == -1.f);
  CHECK(s32_to_float(1 << 30) == 0.5f);
  CHECK(s32_to_float(2147483647) == 1.f);  // rounds to the nearest float
  for (int32_t v : {1, -1, 12345678, -87654321, 1 << 24})
    CHECK(s32_to_float(v) == float(double(v) / 2147483648.0));
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
  const int64_t deadline = mono_ns() + 40000000000LL;  // one servo update per second
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
  for (int i = 0; i < 5; ++i) {
    local += 250000000LL, master += 250000000LL;
    const auto r = servo.sample(local, master + 5000000);
    if (i < 4) CHECK(r == PiServo::kOutlier);
    else CHECK(r == PiServo::kStep);
  }
}

TEST(servo_rides_through_transient_ms_offset) {
  // Three windows see the master 1.1 ms off (path-delay or timestamp
  // disturbance), then it is back: no step either way (each would be a
  // dropout), and the model stays where it was.
  PiServo servo;
  int64_t local = 1000000000LL, master = 1700000000000000000LL;
  for (int i = 0; i < 80; ++i, local += 250000000LL, master += 250000000LL) servo.sample(local, master);
  CHECK(servo.locked());
  int steps = 0;
  for (int i = 0; i < 3; ++i, local += 250000000LL, master += 250000000LL)
    steps += servo.sample(local, master + 1100000) == PiServo::kStep;
  for (int i = 0; i < 20; ++i, local += 250000000LL, master += 250000000LL)
    steps += servo.sample(local, master) == PiServo::kStep;
  CHECK(steps == 0);
  CHECK(std::llabs(servo.model().ptp_at(local) - master) < 50000);
}

TEST(servo_gate_recovers_from_lasting_shift) {
  // A real 80 us phase change just outside the outlier envelope must be
  // followed (not rejected forever while the model drifts).
  PiServo servo;
  int64_t local = 1000000000LL, master = 1700000000000000000LL;
  for (int i = 0; i < 80; ++i, local += 250000000LL, master += 250000000LL) servo.sample(local, master);
  CHECK(servo.locked());
  for (int i = 0; i < 200; ++i, local += 250000000LL, master += 250000000LL) servo.sample(local, master + 80000);
  CHECK(std::llabs(servo.model().ptp_at(local) - (master + 80000)) < 10000);
}

TEST(servo_locks_with_large_frequency_offset) {
  // One servo update per second (4 Sync/s, best of 4), as PtpClock feeds it.
  // A relative offset of 120 ppm (VM clocks, hosts without NTP) used to
  // step again and again because the frequency seeding kept being postponed.
  for (double ppm : {20.0, 120.0, -150.0}) {
    PiServo s;
    ClockModel start;
    start.base_local = 5000000000LL;
    start.base_ptp = 1000;  // far away: first step
    s.reset_to(start);
    const int64_t master0 = 1700000000000000000LL;
    int steps = 0, lock_at = -1;
    for (int i = 0; i < 300; ++i) {
      const int64_t master = master0 + int64_t(i) * 1000000000LL;
      const int64_t local = 5000000000LL + int64_t(double(master - master0) * (1.0 + ppm * 1e-6));
      if (s.sample(local, master) == PiServo::kStep) ++steps;
      if (lock_at < 0 && s.locked()) lock_at = i;
    }
    CHECK(steps <= 2);
    CHECK(lock_at > 0 && lock_at < 120);
  }
}

TEST(clock_model_cell_always_readable) {
  ClockModelCell c;
  CHECK(c.load().ratio == 1.0);
  for (int i = 0; i < 5; ++i) {
    ClockModel m;
    m.base_local = i;
    m.base_ptp = 10 * i;
    m.ratio = 1.0 + i * 1e-6;
    c.store(m);
    const ClockModel r = c.load();
    CHECK(r.base_local == i && r.base_ptp == 10 * i && r.ratio == m.ratio);
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
  Config big;
  parse_config("[device]\ntx_channels = 128\nrx_channels = 128\n", &big, &err);
  CHECK(validate_config(&big, &err));
  CHECK(shm_total_bytes(big.ring_frames, 128, 128) > kShmHeaderBytes);
  Config too_big;
  parse_config("[device]\ntx_channels = 129\n", &too_big, &err);
  CHECK(!validate_config(&too_big, &err));
}

TEST(config_stock_name_becomes_unique) {
  const std::string n = host_device_name();
  CHECK(n.rfind("Virgil", 0) == 0 && n.size() <= 31);
  for (char ch : n) CHECK(std::isalnum(uint8_t(ch)) || ch == '-');
  Config c;
  std::string err;
  c.device_name = n;
  CHECK(validate_config(&c, &err));

  const std::string path = "virgil-name-test.conf";
  auto write = [&](const char* text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fputs(text, f);
    std::fclose(f);
  };
  auto read = [&]() {
    std::string s;
    FILE* f = std::fopen(path.c_str(), "rb");
    char b[512];
    size_t k;
    while ((k = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, k);
    std::fclose(f);
    return s;
  };
  write("# name = Virgil\r\n[device]\r\nname = \"Virgil\"\r\nrx_channels = 4\r\n[ptp]\r\nname = Virgil\r\n");
  CHECK(replace_stock_name(path, "Virgil-host", &err));
  CHECK(read() == "# name = Virgil\r\n[device]\r\nname = Virgil-host\r\nrx_channels = 4\r\n[ptp]\r\nname = Virgil\r\n");
  write("[device]\nname = Stage Left\n");
  CHECK(!replace_stock_name(path, "Virgil-host", &err));
  CHECK(read() == "[device]\nname = Stage Left\n");
  std::remove(path.c_str());
}

TEST(config_channel_names) {
  Config c;
  std::string err;
  CHECK(parse_config("[device]\ntx_channels = 4\nrx_channels = 2\n[channel_names]\ntx1 = Kick\ntx3 = \"Vox; lead\"\n"
                     "rx2 = Talkback\n",
                     &c, &err));
  CHECK(validate_config(&c, &err));
  CHECK(c.tx_name(0) == "Kick" && c.tx_name(1).empty() && c.tx_name(2) == "Vox; lead" && c.rx_name(1) == "Talkback");
  // Round trip through format_config.
  Config d;
  CHECK(parse_config(format_config(c), &d, &err) && validate_config(&d, &err));
  CHECK(d.tx_name(2) == "Vox; lead" && d.rx_name(1) == "Talkback");
  // Unique per direction, also against the default names; bad characters.
  Config dup;
  parse_config("[device]\ntx_channels = 2\n[channel_names]\ntx1 = Kick\ntx2 = kick\n", &dup, &err);
  CHECK(!validate_config(&dup, &err));
  Config dflt;
  parse_config("[device]\ntx_channels = 2\n[channel_names]\ntx1 = TX 2\n", &dflt, &err);
  CHECK(!validate_config(&dflt, &err));
  CHECK(!channel_name_problem("a@b").empty() && !channel_name_problem("a.b").empty() &&
        !channel_name_problem(std::string(32, 'x')).empty() && channel_name_problem("Vox 1 (L)").empty());
  // Names of channels beyond the count are dropped.
  Config extra;
  parse_config("[device]\ntx_channels = 1\n[channel_names]\ntx5 = Five\n", &extra, &err);
  CHECK(validate_config(&extra, &err) && extra.tx_names.size() == 1);
  CHECK(!parse_config("[channel_names]\nfoo = 1\n", &extra, &err));

  // Saving one name keeps the rest of the file.
  const std::string path = "virgil-names-test.conf";
  {
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fputs("# my setup\n[device]\nname = Stage\n\n[ptp]\nsubdomain = _DFLT\n", f);
    std::fclose(f);
  }
  CHECK(save_channel_name(path, true, 0, "Kick", &err));
  CHECK(save_channel_name(path, false, 3, "Talk back", &err));
  CHECK(save_channel_name(path, true, 0, "Kick In", &err));
  Config s;
  CHECK(load_config(path, &s, &err));
  CHECK(s.device_name == "Stage" && s.tx_name(0) == "Kick In" && s.rx_name(3) == "Talk back");
  // A value that would end its line or quotes is refused (config injection).
  CHECK(!save_channel_name(path, true, 1, "x\"\n[device]\nshm_name = /evil", &err));
  CHECK(!set_config_value(path, "device", "name", "a\nb", &err));
  CHECK(save_channel_name(path, true, 0, "", &err));  // back to the default
  Config s2;
  CHECK(load_config(path, &s2, &err) && s2.tx_name(0).empty() && s2.rx_name(3) == "Talk back");
  std::remove(path.c_str());
}

TEST(device_channel_names) {
  std::string err;
  // No names configured: the device keeps the names it saved.
  Config none;
  CHECK(parse_config("[device]\ntx_channels = 3\nrx_channels = 2\n", &none, &err) && validate_config(&none, &err));
  auto t = device_channel_names(none, true), r = device_channel_names(none, false);
  CHECK(t.size() == 3 && r.size() == 2);
  for (const auto& n : t) CHECK(n.empty());
  for (const auto& n : r) CHECK(n.empty());
  // Any name configured: every channel of both directions is named, so a
  // saved "Kick" on tx2 cannot duplicate the configured tx1.
  Config some;
  CHECK(parse_config("[device]\ntx_channels = 3\nrx_channels = 2\n[channel_names]\ntx1 = Kick\n", &some, &err) &&
        validate_config(&some, &err));
  t = device_channel_names(some, true);
  r = device_channel_names(some, false);
  CHECK(t.size() == 3 && t[0] == "Kick" && t[1] == "TX 2" && t[2] == "TX 3");
  CHECK(r.size() == 2 && r[0] == "RX 1" && r[1] == "RX 2");
  // The defaults are only given to the device, not added to the configuration.
  CHECK(some.tx_name(1).empty() && some.rx_names.empty());
  CHECK(format_config(some).find("TX 2") == std::string::npos);
  // A network rename saved in the configuration is passed on next start.
  some.rx_names.resize(2);
  some.rx_names[1] = "Talkback";
  CHECK(validate_config(&some, &err));
  r = device_channel_names(some, false);
  CHECK(r[0] == "RX 1" && r[1] == "Talkback");
}

TEST(diagnostics_events_and_hints) {
  Config cfg;
  std::string err;
  CHECK(validate_config(&cfg, &err));
  Diagnostics d;
  int64_t t = 1700000000000LL;
  d.engine_started(cfg, t);
  EngineCounters c;
  c.state = kStatePtpLocked;
  c.grandmaster = "00:1d:c1:aa:bb:cc";
  EngineCounters::App app;
  app.pid = 42;
  app.name = "REAPER";
  c.apps.push_back(app);
  for (int i = 0; i < 5; ++i) d.sample(c, cfg, t += 1000);  // past the start-up grace
  CHECK(d.recent_dropouts(t) == 0);
  // Two seconds of lost late audio merge into one event; the hint asks for more latency.
  c.rx_late_packets += 10;
  c.rx_lost_late_samples += 48;
  c.rx_max_age_samples = 48 * 5;  // 5 ms at 48 kHz, latency 4 ms
  d.sample(c, cfg, t += 1000);
  c.rx_late_packets += 3;
  c.rx_lost_late_samples += 96;
  d.sample(c, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 1);
  std::string j = d.json(t);
  CHECK(j.find("3.0 ms of audio from the network arrived too late and was lost (packets up to 5.0 ms old") !=
        std::string::npos);
  CHECK(j.find("Raise Receive latency to 10 ms") != std::string::npos);
  // An app that missed its deadline; lost network audio; a lost clock leader.
  c.apps[0].underrun_ticks += 3;
  c.rx_gaps += 2;
  c.rx_missing_samples += 96;
  c.rx_max_age_samples = 0;
  d.sample(c, cfg, t += 10000);
  c.state = kStateFreeRun;
  c.grandmaster.clear();  // as PtpClock does when the leader is gone
  d.sample(c, cfg, t += 1000);
  j = d.json(t);
  CHECK(j.find("REAPER did not deliver its audio in time (3.0 ms of silence sent)") != std::string::npos);
  CHECK(j.find("2.0 ms of audio from the network never arrived (2 gaps)") != std::string::npos);
  CHECK(j.find("Lost the Dante clock leader 00:1d:c1:aa:bb:cc") != std::string::npos);
  CHECK(j.find("REAPER did not keep up") != std::string::npos);
  CHECK(d.recent_dropouts(t) == 4);
  c.late_ticks += 50;  // slightly late wake-ups are absorbed by the margin
  d.sample(c, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 4);
  c.overdue_ticks += 2;
  d.sample(c, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 5);
  EngineCounters idle = c;  // the same with no app attached: nothing to drop
  idle.apps.clear();
  idle.overdue_ticks += 5;
  d.sample(idle, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 5);
  CHECK(d.report(t).find("[network-late]") != std::string::npos);
  // Free-running while the leader is still known is a re-lock after a clock
  // step, not a lost leader.
  {
    Diagnostics d2;
    d2.engine_started(cfg, t);
    EngineCounters k;
    k.state = kStatePtpLocked;
    k.grandmaster = "00:1d:c1:aa:bb:cc";
    for (int i = 0; i < 5; ++i) d2.sample(k, cfg, t += 1000);
    k.state = kStateFreeRun;
    d2.sample(k, cfg, t += 1000);
    CHECK(d2.json(t).find("Lost the Dante clock leader") == std::string::npos);
  }
  // Counters that restart from zero (new engine) are not negative deltas.
  d.engine_started(cfg, t += 1000);
  EngineCounters fresh;
  fresh.state = kStateFreeRun;
  for (int i = 0; i < 5; ++i) d.sample(fresh, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 5);
  d.reset(t);
  CHECK(d.recent_dropouts(t) == 0);
}

TEST(diagnostics_late_network_send) {
  Config cfg;
  std::string err;
  CHECK(validate_config(&cfg, &err));
  Diagnostics d;
  int64_t t = 1700000000000LL;
  d.engine_started(cfg, t);
  EngineCounters c;
  c.state = kStatePtpLocked;
  c.grandmaster = "00:1d:c1:aa:bb:cc";
  EngineCounters::App app;
  app.pid = 7;
  app.name = "REAPER";
  c.apps.push_back(app);
  c.tx_late_wakeups = 4;  // before this engine (process-wide totals): baseline
  for (int i = 0; i < 5; ++i) d.sample(c, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 0);
  // Two consecutive seconds of late sends: one event, the worst lag kept.
  c.tx_late_wakeups += 2;
  c.tx_dropped_samples += 600;
  c.tx_max_lag_samples = 48 * 6;  // 6 ms at 48 kHz
  d.sample(c, cfg, t += 1000);
  c.tx_late_wakeups += 1;
  c.tx_max_lag_samples = 48 * 5;
  d.sample(c, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 1);
  const std::string j = d.json(t);
  char want[160];
  std::snprintf(want, sizeof want,
                "Audio to the network was dropped: Virgil's sending thread woke up to 6.0 ms late 3 times "
                "(transmit latency is %.1f ms)",
                cfg.tx_latency_us / 1000.0);
  CHECK(j.find(want) != std::string::npos);
  CHECK(j.find("\"kind\":\"network-send-late\"") != std::string::npos);
  CHECK(j.find("\"network-send-late\":1") != std::string::npos);  // totals
  CHECK(j.find("Raise Transmit latency to 10 ms") != std::string::npos);
  CHECK(j.find("\"engine\":2") != std::string::npos);  // per-minute: computer busy
  CHECK(d.report(t).find("[network-send-late]") != std::string::npos);
  CHECK(d.report(t).find("worst late send 6.0 ms") != std::string::npos);
  // With no app attached nothing audible was dropped.
  c.apps.clear();
  c.tx_late_wakeups += 5;
  d.sample(c, cfg, t += 10000);
  CHECK(d.recent_dropouts(t) == 1);
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

TEST(control_conf_aliases_and_last_wins) {
  // Virgil Control reads the file as the daemon does.
  const std::string t =
      "[device]\nname = A\ntx_channels = 2\n[network]\nplayback_channels = 6\ncapture_channels = 0\n"
      "rx_latency_us = 2000\n[ptp]\nmaster_capable = Yes\n[device]\nclock = FREE\n";
  Config c;
  std::string err;
  CHECK(parse_config(t, &c, &err));
  CHECK(vc::conf_get(t, "device", "tx_channels") == "6" && c.tx_channels == 6);
  CHECK(vc::conf_get(t, "device", "rx_channels") == "0" && c.rx_channels == 0);
  CHECK(vc::conf_get(t, "device", "latency_us") == "2000" && c.latency_us == 2000);
  const vc::SettingsForm f = vc::load_settings(t, {});
  CHECK(f.tx_channels == 6 && f.rx_channels == 0 && f.rx_latency_ms == "2");
  CHECK(f.master_capable && f.local_clock);
  CHECK(vc::load_settings("[device]\n", {}).tx_channels == 8);
  CHECK(!vc::load_settings("[ptp]\nmaster_capable = off\n", {}).master_capable);

  // Saving changes every occurrence, so the daemon reads the new value.
  vc::SettingsForm g = f;
  g.tx_channels = 3;
  g.rx_latency_ms = "5";
  const std::string out = vc::apply_settings(t, g);
  Config d;
  CHECK(parse_config(out, &d, &err));
  CHECK(d.tx_channels == 3 && d.latency_us == 5000 && d.rx_channels == 0);
  CHECK(out.find("playback_channels = 3") != std::string::npos);
  // A key that is not there yet goes into [device], or [network] without one.
  CHECK(vc::conf_set("[network]\nname = B\n", "device", "interface", "eth0") ==
        "[network]\nname = B\ninterface = eth0\n");
}

TEST(control_tooltip_and_utf8) {
  vc::Status s;
  s.reachable = true;
  s.name = "V";
  s.state = "waiting-for-network";
  CHECK(vc::tray_tooltip(s).find("Waiting for network") != std::string::npos);
  s.state = "restarting";
  CHECK(vc::tray_tooltip(s).find("Restarting") != std::string::npos);
  s.state = "running";
  s.dante = true;
  s.clock = "free-run";
  s.clock_source = "free";
  s.clients = {{"a", true}};
  s.dropouts_last_hour = 2;
  const std::string t = vc::tray_tooltip(s);
  CHECK(t.find("Local clock only") != std::string::npos);
  CHECK(t.find("1 app playing") != std::string::npos && t.find("2 dropouts") != std::string::npos);

  CHECK(vc::valid_utf8("ok \xC3\xA9") == "ok \xC3\xA9");
  CHECK(vc::valid_utf8("cut \xE2\x82") == "cut \xEF\xBF\xBD\xEF\xBF\xBD");
  CHECK(vc::valid_utf8("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");  // overlong
  vc::Status p;
  CHECK(vc::parse_status("{\"device\":{\"name\":\"X\\ud83c\\udfb5\"},\"clients\":[{\"name\":\"ab\xE2\"}]}", &p));
  CHECK(p.name == "X\xF0\x9F\x8E\xB5" && p.clients.size() == 1 && p.clients[0].name == "ab\xEF\xBF\xBD");
}

TEST(config_latency_errors_in_ms) {
  Config c;
  std::string err;
  parse_config("[device]\nlatency_us = 100\n", &c, &err);
  CHECK(!validate_config(&c, &err) && err.find("0.5 ms") != std::string::npos);
  Config t;
  parse_config("[device]\ntx_latency_us = 1000\n", &t, &err);
  CHECK(!validate_config(&t, &err) && err.find("3.5 ms") != std::string::npos);
}

TEST(control_no_clock_leader_notice) {
  // Older daemons send no "clock.ptp": no notice, parse still fine.
  const std::string base =
      "{\"status\":\"running\",\"device\":{\"name\":\"V\",\"interface\":\"\",\"address\":\"192.168.1.5\","
      "\"dante\":true},\"clock\":{\"state\":\"free-run\",\"source\":\"ptp\",\"grandmaster\":\"\"";
  vc::Status old;
  CHECK(vc::parse_status(base + "}}", &old));
  CHECK(!old.ptp_info && !vc::clock_notice(old).show);

  vc::Status s;
  CHECK(vc::parse_status(base + ",\"ptp\":{\"senders\":0,\"master_heard\":false,\"no_leader_s\":16.2,"
                                "\"adapter\":\"eth0\"}}}",
                         &s));
  CHECK(s.ptp_info && s.ptp_senders == 0 && !s.ptp_master_heard && s.ptp_adapter == "eth0");
  CHECK_NEAR(s.no_leader_s, 16.2, 1e-9);
  vc::ClockNotice n = vc::clock_notice(s);
  CHECK(n.show && n.title == "No Dante clock leader found on eth0 (192.168.1.5)");
  CHECK(n.hints.size() == 3 && n.hints[1].find("UDP ports 319 and 320") != std::string::npos);
  CHECK(n.hints[2].find("Become clock leader when no Dante device provides one") != std::string::npos);
  CHECK(vc::tray_tooltip(s).find("No Dante clock leader found on eth0") != std::string::npos);

  // The window shows the title and the hints (and still fits them).
  struct Rec : vc::Canvas {
    std::vector<std::string> texts;
    void fill(const vc::Rect&, unsigned, float) override {}
    void fill_round(const vc::Rect&, float, unsigned, float) override {}
    void stroke_round(const vc::Rect&, float, unsigned) override {}
    void hline(float, float, float, unsigned) override {}
    void dot(float, float, float, unsigned) override {}
    void text(const std::string& t, vc::Font, const vc::Rect& r, unsigned, vc::Align) override {
      if (r.b <= r.t || r.r <= r.l) texts.push_back("<empty rect>" + t);
      texts.push_back(t);
    }
    float text_width(const std::string& t, vc::Font) override { return float(t.size()) * 6; }
    void logo(const vc::Rect&) override {}
  };
  auto painted = [](const vc::Status& st) {
    vc::StatusView v;
    v.update(st, 1000);
    Rec r;
    v.paint(r, v.width(), v.height());
    std::string all;
    for (const auto& t : r.texts) all += t + "\n";
    return all;
  };
  std::string all = painted(s);
  CHECK(all.find("No Dante clock leader found on eth0 (192.168.1.5)") != std::string::npos);
  CHECK(all.find("Settings \xE2\x80\xBA Network interface") != std::string::npos);
  CHECK(all.find("<empty rect>") == std::string::npos);

  // Not yet 15 s, locked, leading, stopped or clock = free: no notice.
  vc::Status q = s;
  q.no_leader_s = 10;
  CHECK(!vc::clock_notice(q).show && painted(q).find("No Dante clock leader found") == std::string::npos);
  q = s, q.clock = "ptp-locked";
  CHECK(!vc::clock_notice(q).show);
  q = s, q.clock = "ptp-master";
  CHECK(!vc::clock_notice(q).show);
  q = s, q.state = "stopped";
  CHECK(!vc::clock_notice(q).show);
  q = s, q.clock_source = "free";
  CHECK(!vc::clock_notice(q).show && vc::tray_tooltip(q).find("Local clock only") != std::string::npos);

  // Devices heard, none leading; a leader that went away.
  q = s, q.ptp_senders = 2;
  n = vc::clock_notice(q);
  CHECK(n.show && n.title == "Dante devices found on eth0 (192.168.1.5), but none is the clock leader yet");
  q.ptp_master_heard = true;
  CHECK(vc::clock_notice(q).title == "Dante clock leader lost on eth0 (192.168.1.5)");
  // No adapter name from the daemon: the configured interface, else generic.
  q = s, q.ptp_adapter.clear();
  CHECK(vc::clock_notice(q).title == "No Dante clock leader found on this adapter (192.168.1.5)");

  // PtpClock's read-only counters, alone on loopback: nothing heard yet.
  PtpClock alone;
  PtpClock::Options o;
  o.interface_addr = 0x7F000001;
  if (alone.start(o)) {
    sleep_until_ns(mono_ns() + 50000000LL);
    CHECK(!alone.master_ever_heard());
    CHECK(alone.seconds_without_master() > 0.0);
    alone.stop();
  }
}

TEST(diagnostics_near_late_packets_and_lost_late_audio) {
  Config cfg;
  std::string err;
  CHECK(validate_config(&cfg, &err));
  Diagnostics d;
  int64_t t = 1700000000000LL;
  d.engine_started(cfg, t);
  EngineCounters c;
  c.state = kStatePtpLocked;
  c.grandmaster = "00:1d:c1:aa:bb:cc";
  for (int i = 0; i < 5; ++i) d.sample(c, cfg, t += 1000);
  // Packets older than the receive latency that the second copy still
  // played: a statistic, not a dropout, and no latency hint.
  c.rx_late_packets += 7;
  c.rx_max_age_samples = 48 * 5;
  d.sample(c, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 0);
  std::string j = d.json(t);
  CHECK(j.find("\"network_late_packets\":7") != std::string::npos);
  CHECK(j.find("Raise Receive latency") == std::string::npos);
  CHECK(d.report(t).find("7 network packets older than the receive latency, still played") != std::string::npos);
  // Audio that arrived after its last copy was lost: a dropout, with a hint
  // from the worst age seen.
  c.rx_max_age_samples = 0;
  c.rx_lost_late_samples += 24;
  d.sample(c, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 1);
  j = d.json(t);
  CHECK(j.find("0.5 ms of audio from the network arrived too late and was lost (receive latency") !=
        std::string::npos);
  CHECK(j.find("Raise Receive latency to 10 ms") != std::string::npos);
  // Sending thread near misses: in the report and JSON, not dropouts.
  c.tx_max_wakeup_lag_samples = 48 * 3;
  c.tx_lag_over[0] += 5;
  c.tx_lag_over[1] += 3;
  c.tx_lag_over[2] += 1;
  d.sample(c, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 1);
  CHECK(d.json(t).find("\"over_1ms\":3") != std::string::npos);
  CHECK(d.report(t).find("worst 3.0 ms; over 0.5 ms 5, over 1 ms 3, over 2 ms 1, over 4 ms 0") !=
        std::string::npos);
}

TEST(diagnostics_engine_stall_is_one_episode_and_not_the_apps_fault) {
  Config cfg;
  std::string err;
  CHECK(validate_config(&cfg, &err));
  Diagnostics d;
  int64_t t = 1700000000000LL;
  d.engine_started(cfg, t);
  EngineCounters c;
  c.state = kStatePtpLocked;
  c.grandmaster = "00:1d:c1:aa:bb:cc";
  EngineCounters::App app;
  app.pid = 9;
  app.name = "REAPER";
  c.apps.push_back(app);
  for (int i = 0; i < 5; ++i) d.sample(c, cfg, t += 1000);
  // One 45 ms stall: 44 overdue catch-up ticks, one episode; the app missed
  // those ticks too, in this second and the next.
  c.overdue_ticks += 44;
  c.overdue_episodes += 1;
  c.max_late_ns = 45000000;
  c.apps[0].underrun_ticks += 40;
  d.sample(c, cfg, t += 1000);
  c.max_late_ns = 0;
  c.apps[0].underrun_ticks += 4;
  c.apps[0].late_frames += 96;
  d.sample(c, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 1);
  std::string j = d.json(t);
  CHECK(j.find("The computer stalled ~45 ms: Virgil's audio engine and apps waited") != std::string::npos);
  CHECK(j.find("REAPER did not deliver") == std::string::npos);
  CHECK(j.find("\"engine-late\":1") != std::string::npos);
#if defined(_WIN32)
  CHECK(j.find("LatencyMon") != std::string::npos);
#endif
  CHECK(d.report(t).find("longest computer stall 45.0 ms") != std::string::npos);
  // Later, on its own, the app is charged again.
  c.apps[0].underrun_ticks += 2;
  d.sample(c, cfg, t += 5000);
  CHECK(d.json(t).find("REAPER did not deliver its audio in time (2.0 ms") != std::string::npos);
  // Two more stalls in consecutive seconds merge, the longest kept.
  c.overdue_episodes += 1;
  c.overdue_ticks += 3;
  c.max_late_ns = 3000000;
  d.sample(c, cfg, t += 5000);
  c.overdue_episodes += 1;
  c.overdue_ticks += 70;
  c.max_late_ns = 70000000;
  d.sample(c, cfg, t += 1000);
  CHECK(d.json(t).find("The computer stalled 2 times, up to ~70 ms") != std::string::npos);
}

namespace {
// A clock for engine tests that can step or hold up the engine's thread (its
// model() is called by the tick thread every tick).
struct TestClock : ClockSource {
  TestClock() {
    ClockModel m;
    m.base_local = mono_ns();
    m.base_ptp = 1000000000000LL;
    cell.store(m);
  }
  ClockModel model() const override {
    // Only the engine's tick thread calls model() once running.
    if (const int64_t ns = stall_ns.exchange(0)) sleep_until_ns(mono_ns() + ns);
    return cell.load();
  }
  DaemonState state() const override { return kStateFreeRun; }
  ClockModelCell cell;
  mutable std::atomic<int64_t> stall_ns{0};
};

Config engine_test_config(const char* tag) {
  Config c;
  c.device_name = "Unit";
  c.interface = "127.0.0.1";
#if defined(_WIN32)
  c.shm_name = std::string(kFallbackShmName) + "-unit-" + tag + std::to_string(process_id());
#else
  // Short: macOS limits shared memory names to 31 characters.
  c.shm_name = std::string("/vgu-") + tag + std::to_string(process_id());
#endif
  c.clock = "free";
  c.lock_memory = false;
  c.tx_channels = 2;
  c.rx_channels = 2;
  return c;
}

uint64_t wait_next_tick(ShmHeader* h) {
  const uint64_t n0 = h->now_frames.load();
  for (int64_t until = mono_ns() + 1000000000LL; mono_ns() < until;) {
    const uint64_t n = h->now_frames.load();
    if (n != n0) return n;
  }
  return 0;
}
}  // namespace

TEST(engine_stall_skips_ahead_and_clock_step_realigns) {
  Config cfg = engine_test_config("stall");
  Engine e(cfg);
  e.set_dante_enabled(false);
  auto clk = std::make_unique<TestClock>();
  TestClock* tc = clk.get();
  CHECK(e.start(std::move(clk)));
  if (!e.running()) return;
  sleep_until_ns(mono_ns() + 100000000LL);
  ShmHeader* h = e.header();
  const uint64_t steps0 = h->clock_steps.load();
  const EngineCounters c0 = e.counters();
  CHECK(steps0 == 1 && c0.resyncs == 0);
  // 120 ms stall on an unchanged clock: skipped, not a clock step.
  tc->stall_ns = 120000000;
  sleep_until_ns(mono_ns() + 300000000LL);
  const EngineCounters c1 = e.counters();
  CHECK(c1.stalls == c0.stalls + 1);
  CHECK(c1.resyncs == c0.resyncs);
  CHECK(h->clock_steps.load() == steps0);
  CHECK(c1.overdue_episodes >= c0.overdue_episodes + 1);
  CHECK(c1.max_late_ns >= 100000000);
  // A real clock step (+1 s) realigns the drivers.
  ClockModel m = tc->cell.load();
  m.base_ptp += 1000000000LL;
  tc->cell.store(m);
  sleep_until_ns(mono_ns() + 100000000LL);
  const EngineCounters c2 = e.counters();
  CHECK(c2.resyncs == c1.resyncs + 1);
  CHECK(c2.stalls == c1.stalls);
  CHECK(h->clock_steps.load() == steps0 + 1);
  e.stop();
}

TEST(engine_rx_second_copy_and_lost_late_audio) {
  Config cfg = engine_test_config("rx");
  std::string err;
  CHECK(validate_config(&cfg, &err));
  Engine e(cfg);
  e.set_dante_enabled(false);
  CHECK(e.start());
  if (!e.running()) return;
  sleep_until_ns(mono_ns() + 50000000LL);
  ShmHeader* h = e.header();
  const uint32_t P = cfg.period_frames(), ch = cfg.rx_channels;
  const uint64_t mask = cfg.ring_frames - 1;
  auto& drx = e.dante_rx_ring();
  const float* cap = rx_ring_at(h, cfg.ring_frames, cfg.tx_channels);
  const int32_t v = 0x10000000;
  // Written after the tick that first copied it: the next tick copies it
  // again, so the apps still get it.
  bool saved = false;
  for (int attempt = 0; attempt < 20 && !saved; ++attempt) {
    const uint64_t n = wait_next_tick(h);
    if (!n) break;
    for (uint64_t f = n - 2 * P; f < n - P; ++f)
      for (uint32_t c = 0; c < ch; ++c) drx[size_t(f & mask) * ch + c] = v;
    if (h->now_frames.load() != n) continue;  // too slow: tick n+P may be done
    sleep_until_ns(mono_ns() + 5000000LL);
    saved = true;
    for (uint64_t f = n - 2 * P; f < n - P; ++f)
      for (uint32_t c = 0; c < ch; ++c) saved &= cap[size_t(f & mask) * ch + c] == s32_to_float(v);
  }
  CHECK(saved);
  // Written into a slot after its last copy: lost, counted, and cleared.
  const uint64_t lost0 = e.counters().rx_lost_late_samples;
  uint64_t lost = lost0;
  for (int attempt = 0; attempt < 20 && lost == lost0; ++attempt) {
    const uint64_t n = wait_next_tick(h);
    if (!n) break;
    sleep_until_ns(mono_ns() + 100000LL);  // tick n has finished its copies
    for (uint64_t f = n - 3 * P; f < n - 2 * P; ++f)
      for (uint32_t c = 0; c < ch; ++c) drx[size_t(f & mask) * ch + c] = v;
    sleep_until_ns(mono_ns() + 5000000LL);
    lost = e.counters().rx_lost_late_samples;
    if (lost != lost0) {
      CHECK(lost - lost0 == P);
      for (uint64_t f = n - 3 * P; f < n - 2 * P; ++f) CHECK(drx[size_t(f & mask) * ch] == 0);
    }
  }
  CHECK(lost == lost0 + P);
  e.stop();
}

// ---- PTP servo: seed, gain schedule, lock criterion, send timestamps -------

TEST(servo_seed_ignores_settling_path_delay) {
  // The first path delays are unfiltered (here 300 us, then the real 30 us):
  // the frequency seed must be measured on t1 alone. Seeding from t1 + delay
  // was 270 us / 8 s = 34 ppm off.
  PiServo s;
  const double ppm = 25.0;
  const int64_t master0 = 1700000000000000000LL;
  for (int i = 0; i < 9; ++i) {  // the seed is taken at the 8 s sample
    const int64_t t1 = master0 + int64_t(i) * 1000000000LL;
    const int64_t d = i < 3 ? 300000 : 30000;
    const int64_t local = 5000000000LL + int64_t(double(t1 + 30000 - master0) * (1.0 + ppm * 1e-6));
    s.sample(local, t1 + d, d);
  }
  const double want = (1.0 / (1.0 + ppm * 1e-6) - 1.0) * 1e9;
  CHECK(std::fabs(s.drift_ppb() - want) < 500);
}

TEST(servo_lock_needs_small_error_and_settled_frequency) {
  // One update per second (best of 4 Sync), 25 ppm, timestamps late by an
  // exponential 40 us plus rare 1 ms spikes (a busy Windows or Pi machine).
  // Lock must come within a minute, and only once the true error is small.
  for (unsigned seed = 1; seed <= 4; ++seed) {
    std::mt19937 rng(seed);
    std::exponential_distribution<double> late(1.0 / 40000.0);
    std::uniform_real_distribution<double> u(0, 1);
    PiServo s;
    ClockModel start;
    start.base_local = 5000000000LL;
    start.base_ptp = 1700000000000000000LL + 50000000;  // 50 ms off: first step
    s.reset_to(start);
    const int64_t master0 = 1700000000000000000LL;
    int lock_at = -1;
    int64_t err_at_lock = 0;
    for (int i = 0; i < 120 && lock_at < 0; ++i) {
      const int64_t master = master0 + int64_t(i) * 1000000000LL;
      const int64_t local = 5000000000LL + int64_t(double(master - master0) * (1.0 + 25e-6));
      int64_t j = int64_t(late(rng));
      if (u(rng) < 0.02) j += 1000000;
      s.sample(local + j, master);
      if (s.locked()) {
        lock_at = i;
        err_at_lock = s.model().ptp_at(local) - master;
      }
    }
    std::printf("  seed %u: locked after %d s, true error %.1f us\n", seed, lock_at,
                err_at_lock / 1000.0);
    CHECK(lock_at >= 10 && lock_at <= 60);  // not before the frequency is known
    CHECK(std::llabs(err_at_lock) < 100000);
  }
}

TEST(servo_gain_schedule_restarts_after_reset) {
  // A 200 us phase error right after reset_to is pulled in by the fast gains
  // well within 30 s; with the slow gains alone it took over a minute.
  PiServo s;
  ClockModel start;
  start.base_local = 5000000000LL;
  start.base_ptp = 1700000000000000000LL;
  s.reset_to(start);
  int64_t local = 6000000000LL, err = 0;
  for (int i = 0; i < 30; ++i, local += 1000000000LL) {
    const int64_t master = start.ptp_at(local) + 200000;
    s.sample(local, master);
    err = s.model().ptp_at(local) - master;
  }
  CHECK(std::llabs(err) < 40000);
}

TEST(udp_tx_timestamps_or_clean_fallback) {
  // Kernel send timestamps where available (Linux), else a clean "none".
  UdpSocket tx, rx;
  if (!tx.open(0, false) || !rx.open(0, false)) return;
  int64_t k = 0;
  CHECK(!tx.read_tx_timestamp(&k, 1));  // not enabled: nothing, at once
  const bool on = tx.enable_tx_timestamps();
  const int64_t t0 = mono_ns();
  CHECK(!tx.read_tx_timestamp(&k, 2));  // enabled but nothing sent: times out
  CHECK(mono_ns() - t0 < 200000000LL);
  int got = 0;
  for (int i = 0; i < 20; ++i) {
    uint8_t b[124] = {};
    const int64_t before = mono_ns();
    // Unbound destination port on loopback: the datagram is still sent.
    CHECK(tx.send_to(b, sizeof b, {0x7F000001, 9}) == int(sizeof b));
    const int64_t after = mono_ns();
    if (tx.read_tx_timestamp(&k, 5)) {
      ++got;
      CHECK(k >= before - 50000 && k <= after + 5000000);
    }
  }
  std::printf("  kernel TX timestamps %s: %d of 20\n", on ? "enabled" : "unavailable", got);
  if (!on) CHECK(got == 0);
  if (on) {
    // A stamp left on the error queue makes select() report the socket
    // readable: the receive that follows must not block.
    uint8_t p[124] = {};
    tx.send_to(p, sizeof p, {0x7F000001, 9});
    sleep_until_ns(mono_ns() + 5000000LL);
    CHECK(UdpSocket::wait_readable(tx, nullptr, 0) & 1);
    uint8_t b[16];
    Endpoint from;
    const int64_t t1 = mono_ns();
    CHECK(tx.recv_from(b, sizeof b, &from) == 0);
    CHECK(mono_ns() - t1 < 100000000LL);
  }
}

// ---- desktop audio: resampler, clock bridge, settings --------------------------------

// Sine of `freq` (cycles per input frame) through the resampler at a fixed
// ratio; returns the SNR (dB) against the ideal signal at the same positions.
static double resampler_snr(double in_rate, double out_rate, double freq_hz, double ppm, double* out_rms_db = nullptr) {
  Resampler rs;
  const double ratio = in_rate / out_rate * (1 + ppm * 1e-6);
  CHECK(rs.init(1, in_rate / out_rate, 512));
  const double w = 2 * 3.14159265358979323846 * freq_hz / in_rate;
  const size_t total_out = size_t(out_rate);  // one second
  std::vector<float> in, out(512);
  uint64_t fed = 0;
  double pos = 0;  // ideal input position of the next output frame
  double sig = 0, err = 0, outp = 0;
  size_t produced = 0;
  while (produced < total_out) {
    const size_t n = 512;
    const size_t need = rs.needed(n, ratio);
    in.resize(need);
    for (size_t i = 0; i < need; ++i) in[i] = float(0.5 * std::sin(w * double(fed + i)));
    fed += need;
    CHECK(rs.push(in.data(), need));
    const size_t got = rs.produce(out.data(), n, ratio);
    CHECK(got == n);
    for (size_t i = 0; i < got; ++i, ++produced) {
      const double ideal = 0.5 * std::sin(w * pos);
      pos += ratio;
      if (produced < 2 * rs.taps()) continue;  // start-up from silence history
      sig += ideal * ideal;
      err += (out[i] - ideal) * (out[i] - ideal);
      outp += double(out[i]) * out[i];
    }
  }
  if (out_rms_db) *out_rms_db = 10 * std::log10(outp / double(total_out) + 1e-30);
  return 10 * std::log10(sig / (err + 1e-30));
}

TEST(resampler_sine_snr) {
  // Drift correction only (48 -> 48 kHz, +200 ppm): music-grade.
  const double s1 = resampler_snr(48000, 48000, 1000, 200);
  const double s2 = resampler_snr(48000, 48000, 12000, -200);
  std::printf("    resampler SNR 1 kHz +200 ppm: %.1f dB, 12 kHz -200 ppm: %.1f dB\n", s1, s2);
  CHECK(s1 > 85);
  CHECK(s2 > 80);
}

TEST(resampler_44k1_to_48k) {
  const double s1 = resampler_snr(44100, 48000, 1000, 0);
  const double s2 = resampler_snr(44100, 48000, 15000, 150);
  const double s3 = resampler_snr(96000, 48000, 5000, 0);
  std::printf("    44.1->48 kHz SNR 1 kHz %.1f dB, 15 kHz %.1f dB; 96->48 kHz 5 kHz %.1f dB\n", s1, s2, s3);
  CHECK(s1 > 85);
  CHECK(s2 > 75);
  CHECK(s3 > 85);
  // Output frame count follows the ratio: 44100 in -> 48000 out.
  Resampler rs;
  CHECK(rs.init(2, 44100.0 / 48000.0, 480));
  std::vector<float> in(44100 * 2, 0.25f), out(480 * 2);
  size_t used = 0, made = 0;
  while (true) {
    const size_t need = rs.needed(480, 44100.0 / 48000.0);
    if (used + need > 44100) break;
    CHECK(rs.push(in.data() + used * 2, need));
    used += need;
    made += rs.produce(out.data(), 480, 44100.0 / 48000.0);
  }
  CHECK(made >= 47400 && made <= 48000);  // all but the last block and the look-ahead
  CHECK_NEAR(out[2 * 479], 0.25, 1e-4);  // DC passes at unity gain
}

TEST(resampler_downsampling_rejects_aliases) {
  // 23 kHz at 48 kHz is above 44.1 kHz's Nyquist frequency: must vanish
  // instead of folding back to 21.1 kHz.
  double rms_db = 0;
  resampler_snr(48000, 44100, 23000, 0, &rms_db);
  std::printf("    48->44.1 kHz, 23 kHz tone (-9 dBFS rms in): %.1f dB rms out\n", rms_db);
  CHECK(rms_db < -80);
  resampler_snr(96000, 48000, 30000, 0, &rms_db);
  CHECK(rms_db < -80);
}

// A device clock (bursty 10 ms packets, delivery jitter, drift) feeding the
// Dante clock (5.3 ms blocks): the bridge must lock to the drift with no
// underrun or overflow, and keep its level.
struct BridgeSimResult {
  uint64_t underruns = 0, overflows = 0;
  double adjust_ppm = 0;    // mean over the last 30 s
  double level_spread_ms = 0;  // max - min level over the last 30 s
};
static BridgeSimResult simulate_bridge(double in_rate, double out_rate, double drift_ppm, double seconds,
                                       size_t packet, size_t block, double jitter_ms,
                                       bool stamp_at_arrival = false) {
  BridgeConfig cfg;
  cfg.channels = 1;
  cfg.in_rate = in_rate;
  cfg.out_rate = out_rate;
  AdaptiveBridge b;
  CHECK(b.init(cfg));
  std::mt19937 rng(1234);
  std::uniform_real_distribution<double> jit(0, jitter_ms * 1e-3);
  const double true_in = in_rate * (1 + drift_ppm * 1e-6);
  std::vector<float> pk(packet), out(block);
  uint64_t k = 0, n = 0;  // packets pushed, blocks pulled
  double next_push = (double(packet) / true_in) + jit(rng);
  BridgeSimResult r;
  double adj_sum = 0, lmin = 1e9, lmax = -1e9;
  uint64_t adj_n = 0;
  uint64_t first_underruns = 0;
  bool running_seen = false;
  while (true) {
    const double next_pull = double(n) * double(block) / out_rate;
    if (next_pull > seconds) break;
    if (next_push <= next_pull) {
      const double end_t = double(k + 1) * double(packet) / true_in;  // packet's last frame (device clock)
      for (size_t i = 0; i < packet; ++i) pk[i] = float(std::sin(double(k * packet + i) * 0.01));
      // Stamped with the device's own time stamp, or (no time stamps from
      // the device) when the packet was picked up.
      b.push(pk.data(), packet, int64_t((stamp_at_arrival ? next_push : end_t) * 1e9));
      ++k;
      next_push = double(k + 1) * double(packet) / true_in + jit(rng);
    } else {
      b.pull(out.data(), block, int64_t(next_pull * 1e9));
      if (!running_seen && b.state() == AdaptiveBridge::kRunning) {
        running_seen = true;
        first_underruns = b.underruns();
      }
      if (next_pull > seconds - 30) {
        adj_sum += b.adjust_ppm();
        ++adj_n;
        lmin = std::min(lmin, b.level_ms());
        lmax = std::max(lmax, b.level_ms());
      }
      ++n;
    }
  }
  r.underruns = b.underruns() - first_underruns;
  r.overflows = b.overflows();
  r.adjust_ppm = adj_n ? adj_sum / double(adj_n) : 0;
  r.level_spread_ms = lmax - lmin;
  return r;
}

TEST(bridge_drift_converges) {
  for (double ppm : {200.0, -200.0}) {
    const BridgeSimResult r = simulate_bridge(48000, 48000, ppm, 180, 480, 256, 2.0);
    std::printf("    48 kHz device %+.0f ppm: adjust %+.1f ppm, level spread %.2f ms, %llu underruns, %llu overflows\n",
                ppm, r.adjust_ppm, r.level_spread_ms, (unsigned long long)r.underruns,
                (unsigned long long)r.overflows);
    CHECK(r.underruns == 0);
    CHECK(r.overflows == 0);
    CHECK_NEAR(r.adjust_ppm, ppm, 5);
    CHECK(r.level_spread_ms < 1.0);
  }
  // Without device time stamps: stamped on pick-up, up to 8 ms late (polling).
  const BridgeSimResult r = simulate_bridge(48000, 48000, 200, 240, 480, 256, 8.0, true);
  std::printf("    stamped on arrival (8 ms jitter): adjust %+.1f ppm, level spread %.2f ms\n", r.adjust_ppm,
              r.level_spread_ms);
  CHECK(r.underruns == 0);
  CHECK(r.overflows == 0);
  CHECK_NEAR(r.adjust_ppm, 200, 20);
}

TEST(bridge_44k1_device_to_48k_with_drift) {
  // 44.1 kHz device -180 ppm into 48 kHz; and the other way (Dante -> device).
  BridgeSimResult r = simulate_bridge(44100, 48000, -180, 180, 441, 256, 3.0);
  std::printf("    44.1 -> 48 kHz -180 ppm: adjust %+.1f ppm, %llu underruns, %llu overflows\n", r.adjust_ppm,
              (unsigned long long)r.underruns, (unsigned long long)r.overflows);
  CHECK(r.underruns == 0);
  CHECK(r.overflows == 0);
  CHECK_NEAR(r.adjust_ppm, -180, 5);
  r = simulate_bridge(48000, 44100, 120, 180, 256, 441, 1.0);
  CHECK(r.underruns == 0);
  CHECK(r.overflows == 0);
  CHECK_NEAR(r.adjust_ppm, 120, 5);
}

TEST(bridge_source_pause_resumes_cleanly) {
  // A loopback source delivers nothing while nothing plays: silence, then a
  // clean restart at the target level, the drift estimate kept.
  BridgeConfig cfg;
  cfg.channels = 1;
  AdaptiveBridge b;
  CHECK(b.init(cfg));
  std::vector<float> pk(480, 0.5f), out(256);
  const double true_in = 48000 * (1 + 150e-6);
  uint64_t k = 0, n = 0;
  bool audio_during_gap = false, audio_after = false;
  while (true) {
    const double t_pull = double(n) * 256 / 48000;
    if (t_pull > 150) break;
    const double t_push = double(k + 1) * 480 / true_in;
    if (t_push <= t_pull) {
      const bool gap = t_push > 100 && t_push < 110;  // 10 s of nothing
      if (!gap) b.push(pk.data(), 480, int64_t(t_push * 1e9));
      ++k;
    } else {
      const bool got = b.pull(out.data(), 256, int64_t(t_pull * 1e9));
      if (t_pull > 100.1 && t_pull < 110) audio_during_gap |= got;
      if (t_pull > 111) audio_after |= !got;  // must be continuous again
      ++n;
    }
  }
  CHECK(!audio_during_gap);
  CHECK(!audio_after);
  CHECK(b.underruns() == 1);
  CHECK_NEAR(b.adjust_ppm(), 150, 10);
}

TEST(desktop_audio_settings_roundtrip_and_robust_parsing) {
  using namespace vc;
  DesktopAudioSettings s;
  s.enabled = true;
  DesktopAudioSource a;
  a.type = DesktopAudioSource::kDevice;
  a.device = "{0.0.0.00000000}.{abc}";
  a.channel = 3;
  a.gain_db = -6.5;
  DesktopAudioSource b;
  b.type = DesktopAudioSource::kApp;
  b.app = "chrome.exe";
  b.mono = true;
  b.mute = true;
  b.channel = 8;
  DesktopAudioSource c;  // an audio interface's inputs 3-10 -> transmit 11-18
  c.type = DesktopAudioSource::kInput;
  c.api = "asio";
  c.device = "Focusrite USB ASIO; #2";
  c.name = "Scarlett 18i20";
  c.first_input = 3;
  c.inputs = 8;
  c.buffer = 128;
  c.channel = 11;
  CHECK(c.width() == 8);
  s.sources = {a, b, c};
  s.output.enabled = true;
  s.output.channel = 5;
  s.output.device = "default";
  const std::string text = format_desktop_audio(s);
  DesktopAudioSettings t;
  std::vector<std::string> warnings;
  CHECK(parse_desktop_audio(text, &t, &warnings));
  CHECK(warnings.empty());
  CHECK(t.sources.size() == 3 && t.sources[2] == c && t.sources[2].width() == 8);
  {
    DesktopAudioSettings u;
    std::vector<std::string> w;
    parse_desktop_audio("[source]\ntype = input\ninputs = 99\nfirst_input = 0\napi = coreaudio\n", &u, &w);
    CHECK(u.sources.size() == 1 && u.sources[0].inputs == kMaxInterfaceInputs && u.sources[0].first_input == 1);
    CHECK(u.sources[0].device == "default" && u.sources[0].api.empty() && w.size() == 3);
  }
  {
    DesktopAudioSettings u = s;  // receive 9-16 -> the interface's outputs 3-10 (same ASIO driver)
    u.output.type = DesktopAudioOutput::kInterface;
    u.output.api = "asio";
    u.output.device = c.device;
    u.output.first_output = 3;
    u.output.outputs = 8;
    u.output.buffer = 128;
    u.output.channel = 9;
    DesktopAudioSettings back;
    std::vector<std::string> w;
    CHECK(parse_desktop_audio(format_desktop_audio(u), &back, &w));
    CHECK(w.empty() && back.output == u.output && back.output.width() == 8);
  }
  CHECK(t.enabled);
  CHECK(t.sources.size() == 3);
  CHECK(t.sources[0].type == DesktopAudioSource::kDevice && t.sources[0].device == a.device);
  CHECK(t.sources[0].channel == 3 && std::fabs(t.sources[0].gain_db + 6.5) < 1e-9);
  CHECK(t.sources[1].type == DesktopAudioSource::kApp && t.sources[1].app == "chrome.exe");
  CHECK(t.sources[1].mono && t.sources[1].mute && t.sources[1].channel == 8);
  CHECK(t.output.enabled && t.output.channel == 5);
  CHECK(t == s);

  // Garbage, unknown keys, bad values, CRLF, BOM, comments: defaults, no crash.
  const std::string messy =
      "\xEF\xBB\xBF# comment\r\n[desktop-audio]\r\nenabled = yes ; trailing\r\nbogus = 1\r\n"
      "[source]\r\ntype = app\r\napp =   Spotify.exe  \r\nchannel = 999\r\ngain_db = loud\r\n"
      "[source]\r\ntype = nonsense\r\n"
      "[source]\r\napp = \r\ntype=app\r\n"
      "[output]\r\nenabled = 1\r\nchannel = -4\r\nmono = true\r\n[unknown]\r\nx=y\r\nno equals sign\r\n";
  DesktopAudioSettings m;
  warnings.clear();
  CHECK(parse_desktop_audio(messy, &m, &warnings));
  CHECK(!warnings.empty());
  CHECK(m.enabled);
  CHECK(m.sources.size() == 1);  // the bad type and the empty app are skipped
  CHECK(m.sources[0].app == "Spotify.exe");
  CHECK(m.sources[0].channel == 128);  // clamped
  CHECK(m.sources[0].gain_db == 0);
  CHECK(m.output.enabled && m.output.channel == 1 && m.output.mono);
  DesktopAudioSettings e;
  CHECK(parse_desktop_audio("", &e, nullptr));
  CHECK(!e.enabled && e.sources.empty() && !e.output.enabled);
}

// The diagnostics window's model, parsed from JSON the daemon's own code
// produces.
TEST(control_diagnostics_model) {
  Config cfg;
  std::string err;
  CHECK(validate_config(&cfg, &err));
  Diagnostics d;
  int64_t t = 1700000000000LL;
  d.engine_started(cfg, t);
  EngineCounters c;
  c.state = kStatePtpLocked;
  c.grandmaster = "00:1d:c1:aa:bb:cc";
  EngineCounters::App app;
  app.pid = 42;
  app.name = "REAPER";
  c.apps.push_back(app);
  for (int i = 0; i < 5; ++i) d.sample(c, cfg, t += 1000);
  c.rx_late_packets += 10;
  c.rx_lost_late_samples += 48;
  c.rx_max_age_samples = 48 * 5;
  d.sample(c, cfg, t += 1000);
  c.rx_late_packets += 3;
  c.rx_lost_late_samples += 96;
  d.sample(c, cfg, t += 1000);  // merged: one event lasting 1 s
  c.apps[0].underrun_ticks += 3;
  c.rx_gaps += 2;
  c.rx_missing_samples += 96;
  c.rx_max_age_samples = 0;
  d.sample(c, cfg, t += 10000);
  c.state = kStateFreeRun;
  c.grandmaster.clear();
  d.sample(c, cfg, t += 1000);

  vc::Diagnostics g;
  CHECK(vc::parse_diagnostics(d.json(t), &g));
  CHECK(g.now_ms == t && g.since_ms == 1700000000000LL);
  CHECK(g.dropouts_last_hour == 4 && g.dropouts_total == 4);
  CHECK(g.headline == "4 dropouts in the last hour" && g.headline_severity == vc::Severity::Warn);
  CHECK(g.since_text == "since " + vc::format_local_time(g.since_ms, "%H:%M:%S") + " \xC2\xB7 4 in total");
  CHECK(g.totals[int(vc::DiagCategory::Network)] == 2 && g.totals[int(vc::DiagCategory::Apps)] == 1);
  CHECK(g.totals[int(vc::DiagCategory::Computer)] == 0 && g.totals[int(vc::DiagCategory::Clock)] == 1);
  CHECK(g.rx_latency_ms == 4.0);
  // 60 minutes, oldest first, ending with the current one.
  CHECK(g.minutes.size() == size_t(vc::kDiagMinutes));
  CHECK(g.minutes.back().t_ms == t - t % 60000 && g.minutes.front().t_ms == g.minutes.back().t_ms - 59 * 60000);
  unsigned sum[vc::kDiagCategories] = {}, peak = 0;
  for (const auto& m : g.minutes) {
    for (int k = 0; k < vc::kDiagCategories; ++k) sum[k] += m.counts[k];
    peak = std::max(peak, m.total());
  }
  CHECK(sum[0] == 3 && sum[1] == 1 && sum[2] == 0 && sum[3] == 1);  // seconds with trouble
  CHECK(g.peak == std::max(1u, peak));
  CHECK(g.minutes.back().label == vc::format_local_time(g.minutes.back().t_ms, "%H:%M"));
  CHECK(g.minutes.back().tooltip.find("clock (seconds with trouble)") != std::string::npos);
  // Events newest first, with severities, durations and local times.
  CHECK(g.events.size() == 6);  // started, locked, late, missed, lost, clock lost
  CHECK(g.events.front().kind == "clock-lost" && g.events.front().severity == vc::Severity::Error &&
        g.events.front().dropout);
  CHECK(g.events.front().text.find("Lost the Dante clock leader 00:1d:c1:aa:bb:cc") == 0);
  CHECK(g.events.front().time_text == vc::format_local_time(g.events.front().start_ms, "%H:%M:%S"));
  CHECK(g.events.back().kind == "service" && !g.events.back().dropout &&
        g.events.back().severity == vc::Severity::Info);
  CHECK(g.events.back().text.find("Audio engine started") == 0);
  bool late = false;
  for (const auto& e : g.events)
    if (e.kind == "network-late") {
      late = true;
      CHECK(e.severity == vc::Severity::Warn && e.duration_text == "1 s");
      CHECK(e.text.find("3.0 ms of audio from the network arrived too late") == 0);
    } else {
      CHECK(e.duration_text.empty());
    }
  CHECK(late);
  bool reaper = false;
  for (const auto& h : g.hints) reaper |= h.find("REAPER did not keep up") != std::string::npos;
  CHECK(reaper && g.hints.size() >= 3);
  vc::DiagCategory cat;
  CHECK(vc::diag_kind_category("network-send-late", &cat) && cat == vc::DiagCategory::Computer);
  CHECK(!vc::diag_kind_category("service", &cat) && !vc::diag_kind_category("clock", &cat));
  CHECK(std::string(vc::diag_category_label(vc::DiagCategory::Computer)) == "computer busy");

  // Chart: one rectangle per non-empty segment, stacked inside the area.
  const auto bars = vc::diag_chart_layout(g, 600, 64);
  size_t segs = 0;
  for (const auto& m : g.minutes)
    for (int k = 0; k < vc::kDiagCategories; ++k) segs += m.counts[k] ? 1 : 0;
  CHECK(bars.size() == segs && segs > 0);
  float tallest = 0;
  for (const auto& b : bars) {
    CHECK(b.x >= 0 && b.x + b.w <= 600.01f && b.y >= -0.01f && b.y + b.h <= 64.01f && b.w > 0);
    if (g.minutes[size_t(b.minute)].total() == g.peak) tallest = std::max(tallest, 64 - b.y);
  }
  CHECK_NEAR(tallest, 64, 0.01);
  CHECK(vc::diag_chart_minute_at(0, 600) == 0 && vc::diag_chart_minute_at(599, 600) == 59 &&
        vc::diag_chart_minute_at(600, 600) == -1);

  // After Clear.
  d.reset(t);
  CHECK(vc::parse_diagnostics(d.json(t), &g));
  CHECK(g.headline == "No dropouts in the last hour" && g.headline_severity == vc::Severity::Ok);
  CHECK(g.events.empty() && g.hints.empty() && g.peak == 1 && g.minutes.size() == 60);
  CHECK(g.since_text == "since " + vc::format_local_time(t, "%H:%M:%S"));
  CHECK(!vc::parse_diagnostics("not json", &g));

  const std::string f = vc::default_report_file_name(1700000000000LL);
  CHECK(f == "virgil-report-" + vc::format_local_time(1700000000000LL, "%Y%m%d-%H%M") + ".txt");
  CHECK(f.size() == std::string("virgil-report-20231114-2213.txt").size());
}

TEST(control_http_header_and_conf_remove) {
  vc::HttpResult r;
  r.headers = "Content-Type: text/plain\r\nX-Virgil-Config-Version:  0123abcd \r\n";
  CHECK(r.header("x-virgil-config-version") == "0123abcd" && r.header("Content-type") == "text/plain");
  CHECK(r.header("X-Virgil").empty());
  r.status = 400;
  r.body = "{\"error\":\"line 3: unknown key 'x'\"}";
  CHECK(vc::http_error(r) == "line 3: unknown key 'x'");
  CHECK(vc::http_error(vc::HttpResult()) == "Could not reach the Virgil service.");
  CHECK(vc::conf_remove("[channel_names]\ntx1 = A\n# tx1 = old\ntx1 = B\nrx1 = C\n", "channel_names", "tx1") ==
        "[channel_names]\n# tx1 = old\nrx1 = C\n");
  CHECK(vc::conf_remove("[device]\nname = A\n", "channel_names", "tx1") == "[device]\nname = A\n");
}

TEST(control_channel_names) {
  const std::string conf =
      "# my setup\n[device]\nname = Stage\ntx_channels = 4\nrx_channels = 2\n\n[channel_names]\ntx1 = Kick\n"
      "tx3 = \"Vox; lead\"\n";
  vc::ChannelNames n = vc::load_channel_names(conf);
  CHECK(n.tx == 4 && n.rx == 2 && n.tx_names.size() == 4 && n.rx_names.size() == 2);
  CHECK(n.tx_names[0] == "Kick" && n.tx_names[1].empty() && n.tx_names[2] == "Vox; lead" && n.rx_names[0].empty());
  CHECK(vc::validate_channel_names(n).empty());
  CHECK(vc::load_channel_names("").tx == 8 && vc::load_channel_names("").tx_names.size() == 8);
  CHECK(vc::default_channel_name(false, 3) == "RX 3");
  CHECK(vc::channel_name_placeholder({"Kick", ""}, true, 1) == "TX 2");
  CHECK(vc::channel_name_placeholder({"Kick", ""}, true, 0) == "Kick");

  // Per-field errors, as the daemon would refuse them.
  vc::ChannelNames e = n;
  e.tx_names[1] = "kick";       // same as channel 1, ignoring case
  e.tx_names[3] = "a@b";
  e.rx_names[0] = "RX 2";       // channel 2's default, but channel 2 is named
  e.rx_names[1] = std::string(32, 'x');
  auto errs = vc::validate_channel_names(e);
  CHECK(errs.size() == 4);
  CHECK(vc::name_error(errs, true, 0) == "also the name of channel 2");
  CHECK(vc::name_error(errs, true, 1) == "also the name of channel 1");
  CHECK(vc::name_error(errs, true, 2).empty());
  CHECK(vc::name_error(errs, true, 3) == "names cannot contain '@'");
  CHECK(vc::name_error(errs, false, 0).empty());
  CHECK(vc::name_error(errs, false, 1) == "names can be at most 31 characters");
  std::string err;
  CHECK(!vc::save_channel_names(1, n, e, &err) && err == "Transmit channel 1: also the name of channel 2");
  e.rx_names[1] = "";  // channel 2 back to its default "RX 2": still a clash
  CHECK(vc::name_error(vc::validate_channel_names(e), false, 0) == "also the name of channel 2");
  vc::ChannelNames sp = n;
  sp.tx_names[1] = "  Snare  ";  // trimmed, like the daemon wants it
  CHECK(vc::validate_channel_names(sp).empty());

  // Apply: the file keeps its comments; an emptied field removes its line.
  vc::ChannelNames ed = n;
  ed.tx_names[0] = "";
  ed.tx_names[1] = " Snare ";
  ed.rx_names[1] = "Talk back";
  const std::string out = vc::apply_channel_names(conf, ed, &n);
  CHECK(out.find("# my setup") == 0 && out.find("Kick") == std::string::npos);
  Config c;
  CHECK(parse_config(out, &c, &err) && validate_config(&c, &err));
  CHECK(c.device_name == "Stage" && c.tx_name(0).empty() && c.tx_name(1) == "Snare" && c.tx_name(2) == "Vox; lead");
  CHECK(c.rx_name(1) == "Talk back");
  const vc::ChannelNames back = vc::load_channel_names(out);
  CHECK(back.tx_names[1] == "Snare" && back.tx_names[2] == "Vox; lead" && back.rx_names[1] == "Talk back");
  // Unchanged: same text. With `base`, a rename made meanwhile in Dante
  // Controller (rx1 here) is kept.
  CHECK(vc::apply_channel_names(conf, n, &n) == conf);
  const std::string fresh = conf + "rx1 = Ambience\n";
  const std::string merged = vc::apply_channel_names(fresh, ed, &n);
  Config m;
  CHECK(parse_config(merged, &m, &err) && m.rx_name(0) == "Ambience" && m.tx_name(1) == "Snare");
  // Without base, every field is written.
  CHECK(vc::load_channel_names(vc::apply_channel_names(fresh, ed)).rx_names[0].empty());

  // The same rules as the daemon: single characters, and random sets of names.
  for (int ch = 1; ch < 256; ++ch) {
    const std::string s(1, char(ch));
    CHECK(vc::channel_name_problem(s) == virgil::channel_name_problem(s));
    const std::string w = "a" + s + "b";
    CHECK(vc::channel_name_problem(w) == virgil::channel_name_problem(w));
  }
  for (const std::string& s : std::vector<std::string>{"", " a", "a ", std::string(31, 'x'), std::string(32, 'x'), "Vox 1 (L)"})
    CHECK(vc::channel_name_problem(s) == virgil::channel_name_problem(s));
  static const char* const pool[] = {"",     "Kick", "kick",  "TX 1", "tx 2", "RX 1", "rx 3",   "Vox; lead",
                                     "a#b",  "a.b",  "x\"y",  " pad ", "@",   "TX 3", "Snare", "Talk back"};
  std::mt19937 rng(7);
  int agree = 0;
  for (int iter = 0; iter < 2000; ++iter) {
    vc::ChannelNames r;
    r.tx = 1 + rng() % 4;
    r.rx = rng() % 4;
    r.tx_names.resize(r.tx);
    r.rx_names.resize(r.rx);
    for (auto& v : r.tx_names) v = pool[rng() % 16];
    for (auto& v : r.rx_names) v = pool[rng() % 16];
    const std::string base = "[device]\ntx_channels = " + std::to_string(r.tx) + "\nrx_channels = " +
                             std::to_string(r.rx) + "\n";
    Config dc;
    std::string de;
    const bool daemon_ok =
        parse_config(vc::apply_channel_names(base, r), &dc, &de) && validate_config(&dc, &de);
    const bool app_ok = vc::validate_channel_names(r).empty();
    CHECK(daemon_ok == app_ok);
    agree += daemon_ok == app_ok;
  }
  CHECK(agree == 2000);
}

TEST(remote_access_auth) {
  // "Basic base64(user:pass)": any user name.
  std::string pw;
  CHECK(basic_auth_password("Basic dmlyZ2lsOnNlY3JldC1wdw==", &pw) && pw == "secret-pw");  // virgil:secret-pw
  CHECK(basic_auth_password("basic OnNlY3JldC1wdw==", &pw) && pw == "secret-pw");        // :secret-pw
  CHECK(!basic_auth_password("Bearer abc", &pw) && !basic_auth_password("Basic !!!", &pw));
  CHECK(!basic_auth_password("Basic c2VjcmV0", &pw));  // "secret": no colon
  CHECK(constant_time_equal("abc", "abc") && !constant_time_equal("abc", "abd") && !constant_time_equal("abc", "ab"));
  CHECK(!constant_time_equal("", "x") && constant_time_equal("", ""));

  RemoteAuth a;
  const uint32_t lan = 0xC0000202, other = 0xC0000203;  // 192.0.2.2, 192.0.2.3
  const std::string good = "Basic dmlyZ2lsOnNlY3JldC1wdw==", bad = "Basic dmlyZ2lsOndyb25n";  // virgil:wrong
  int64_t t = 1000;
  // This computer: no password, also with remote access off.
  CHECK(a.check(0x7F000001, false, "", "", t) == RemoteAuth::Result::Allowed);
  CHECK(a.check(0x7F000002, true, "secret-pw", bad, t) == RemoteAuth::Result::Allowed);
  // Others: refused while off, a login asked for, the password checked.
  CHECK(a.check(lan, false, "secret-pw", good, t) == RemoteAuth::Result::Refused);
  CHECK(a.check(lan, true, "", good, t) == RemoteAuth::Result::Refused);
  CHECK(a.check(lan, true, "secret-pw", "", t) == RemoteAuth::Result::NeedLogin);
  CHECK(a.check(lan, true, "secret-pw", good, t) == RemoteAuth::Result::Allowed);
  // Requests without credentials do not count; wrong passwords do: the 5th
  // locks the address out for 10 s, even with the right password.
  for (int i = 0; i < 10; ++i) CHECK(a.check(lan, true, "secret-pw", "", t) == RemoteAuth::Result::NeedLogin);
  for (int i = 0; i < 4; ++i) CHECK(a.check(lan, true, "secret-pw", bad, t) == RemoteAuth::Result::NeedLogin);
  CHECK(a.check(lan, true, "secret-pw", bad, t) == RemoteAuth::Result::LockedOut);
  CHECK(a.check(lan, true, "secret-pw", good, t + 5000) == RemoteAuth::Result::LockedOut);
  CHECK(a.retry_after_s(lan, t + 5000) == 5);
  CHECK(a.check(other, true, "secret-pw", good, t + 5000) == RemoteAuth::Result::Allowed);  // per address
  CHECK(a.check(lan, true, "secret-pw", good, t + 10000) == RemoteAuth::Result::Allowed);
  // A success clears the count.
  for (int i = 0; i < 4; ++i) a.check(lan, true, "secret-pw", bad, t + 11000);
  CHECK(a.check(lan, true, "secret-pw", good, t + 11000) == RemoteAuth::Result::Allowed);
  CHECK(a.check(lan, true, "secret-pw", bad, t + 11000) == RemoteAuth::Result::NeedLogin);

  // Masking: remote clients see the mask; posting it back keeps the password.
  const std::string conf = "# setup\n[device]\nname = Stage\nremote_access = true\nremote_password = \"secret; pw\"\n"
                           "[ptp]\nsubdomain = _DFLT\n";
  const std::string masked = mask_remote_password(conf);
  CHECK(masked.find("secret") == std::string::npos && masked.find(kPasswordMask) != std::string::npos);
  CHECK(masked.find("# setup") == 0 && mask_remote_password("[device]\nname = A\n") == "[device]\nname = A\n");
  Config mc;
  std::string err, out;
  CHECK(parse_config(masked, &mc, &err) && mc.remote_password == kPasswordMask);
  std::string edited = masked;
  edited.replace(edited.find("name = Stage"), 12, "name = Booth");
  CHECK(resolve_posted_config(edited, conf, false, &out, &err));
  Config rc;
  CHECK(parse_config(out, &rc, &err) && rc.device_name == "Booth" && rc.remote_password == "secret; pw" &&
        rc.remote_access);
  // Remote clients cannot change remote access or its password; this computer can.
  std::string off = masked;
  off.replace(off.find("remote_access = true"), 20, "remote_access = false");
  CHECK(!resolve_posted_config(off, conf, false, &out, &err) && err.find("only be changed") != std::string::npos);
  std::string newpw = conf;
  newpw.replace(newpw.find("secret; pw"), 10, "another-pw");
  CHECK(!resolve_posted_config(newpw, conf, false, &out, &err));
  CHECK(!resolve_posted_config("[device]\nname = Booth\n", conf, false, &out, &err));  // removing them
  CHECK(resolve_posted_config(off, conf, true, &out, &err) && out.find("secret; pw") != std::string::npos);
  CHECK(resolve_posted_config(newpw, conf, true, &out, &err) && out == newpw);

  // The configuration: off by default; on needs a password of 8 or more.
  Config d;
  CHECK(parse_config("[device]\n", &d, &err) && !d.remote_access && validate_config(&d, &err));
  Config on;
  CHECK(parse_config("[device]\nremote_access = true\n", &on, &err));
  CHECK(!validate_config(&on, &err) && err.find("needs a password") != std::string::npos);
  on.remote_password = "short";
  CHECK(!validate_config(&on, &err));
  on.remote_password = "long enough";
  CHECK(validate_config(&on, &err));
  Config back;
  CHECK(parse_config(format_config(on), &back, &err) && back.remote_access && back.remote_password == "long enough");

  // Virgil Control's settings form.
  vc::SettingsForm f = vc::load_settings(conf, {});
  CHECK(f.remote_access && f.remote_password == "secret; pw" && vc::remote_access_problem(f).empty());
  f.remote_password = "x";
  CHECK(!vc::remote_access_problem(f).empty());
  f.remote_password = "new; password";
  Config sc;
  CHECK(parse_config(vc::apply_settings(conf, f), &sc, &err) && sc.remote_password == "new; password");
  // A file without the keys keeps none when remote access stays off.
  const std::string plain = "[device]\nname = A\n";
  CHECK(vc::apply_settings(plain, vc::load_settings(plain, {})).find("remote") == std::string::npos);
}

TEST_MAIN()
