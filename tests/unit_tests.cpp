#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

#include "virgil/client.h"
#include "virgil/config.h"
#include "virgil/media_clock.h"
#include "virgil/platform.h"
#include "virgil/ptp.h"
#include "virgil/sample_convert.h"
#include "virgil/shm_layout.h"
#include "diagnostics.h"
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
  // Two seconds of late packets merge into one event; the hint asks for more latency.
  c.rx_late_packets += 10;
  c.rx_max_age_samples = 48 * 5;  // 5 ms at 48 kHz, latency 4 ms
  d.sample(c, cfg, t += 1000);
  c.rx_late_packets += 3;
  d.sample(c, cfg, t += 1000);
  CHECK(d.recent_dropouts(t) == 1);
  std::string j = d.json(t);
  CHECK(j.find("13 packets from the network arrived too late") != std::string::npos);
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

TEST_MAIN()
