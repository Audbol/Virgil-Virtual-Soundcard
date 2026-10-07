// Soundcard path end to end, with Inferno replaced by a network simulator:
// two playback clients -> mixer -> Dante TX ring --(sent at f + tx latency,
// written by the receiver at + rx latency)--> Dante RX ring -> capture ring
// -> client. Verifies mixing, 32-bit conversion and frame alignment.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "virgil/client.h"
#include "virgil/engine.h"
#include "virgil/log.h"
#include "virgil/media_clock.h"
#include "virgil/platform.h"
#include "test_main.h"

using namespace virgil;

static void wait_ms(int ms) { sleep_until_ns(mono_ns() + int64_t(ms) * 1000000); }

TEST(soundcard_loopback_through_dante_rings) {
  g_log_level = kLogWarn;
  Config c;
  c.device_name = "Loop Test";
  c.interface = "127.0.0.1";
#if defined(_WIN32)
  c.shm_name = std::string(kFallbackShmName) + "-test-" + std::to_string(process_id());
#else
  c.shm_name = std::string(kDefaultShmName) + "-test-" + std::to_string(process_id());
#endif
  c.clock = "free";
  c.lock_memory = false;
  c.tx_channels = 4;
  c.rx_channels = 4;
  c.latency_us = 4000;  // a receiver uses at least the sender's tx latency
  c.tx_latency_us = 4000;
  c.ring_frames = 16384;

  Engine e(c);
  e.set_dante_enabled(false);
  CHECK(e.start());
  if (!e.running()) return;
  wait_ms(50);  // first ticks publish the clock
  // Inferno sends frame f at media time f + send delay, labelled 0.5 ms
  // early; the receiver writes it at label + its latency.
  const uint64_t L = c.us_to_frames(c.tx_send_delay_us()), RL = c.us_to_frames(c.latency_us);
  const uint64_t D = RL - c.us_to_frames(500);
  const uint64_t dmask = c.ring_frames - 1;

  // Network simulator: what Inferno + the wire + a receiving Inferno do.
  std::atomic<bool> sim_run{true};
  std::thread sim([&] {
    auto& tx = e.dante_tx_ring();
    auto& rx = e.dante_rx_ring();
    uint64_t done = 0;
    while (sim_run) {
      wait_ms(1);
      const uint64_t media = e.header()->now_frames.load();
      if (done == 0) done = media - 64;
      const uint64_t sent_until = media > L ? media - L : 0;  // frames sent by now
      for (; done < sent_until; ++done)
        for (uint32_t ch = 0; ch < 4; ++ch)
          rx[size_t((done + D) & dmask) * 4 + ch] = tx[size_t(done & dmask) * 4 + ch];
    }
  });

  Client a, b;
  CHECK(a.open(c.shm_name));
  CHECK(b.open(c.shm_name));
  CHECK(a.daemon_alive());
  CHECK(a.acquire_tx_slot("client-a"));
  CHECK(b.acquire_tx_slot("client-b"));
  a.set_tx_active(true);
  b.set_tx_active(true);

  double now;
  CHECK(a.frame_now(&now));
  const uint64_t start = uint64_t(now) + 480;  // 10 ms ahead
  const uint32_t n = 2400;
  std::vector<float> sa(n * 4), sb(n * 2);
  for (uint32_t f = 0; f < n; ++f) {
    for (uint32_t ch = 0; ch < 4; ++ch)
      sa[f * 4 + ch] = 0.25f * std::sin(0.01f * float(f) * float(ch + 1));
    sb[f * 2 + 0] = 0.125f;  // DC on ch 1 from client b
    sb[f * 2 + 1] = 0.f;
  }
  CHECK(a.write_tx(start, sa.data(), n, 4) == 0);
  CHECK(b.write_tx(start, sb.data(), n, 2) == 0);

  // The block comes back D frames later; wait until it is all readable.
  for (;;) {
    double t;
    a.frame_now(&t);
    if (t > double(start + D + n + a.rx_latency_frames() + 2 * a.period_frames())) break;
    wait_ms(5);
  }
  std::vector<float> got(n * 4);
  a.read_rx(start + D, got.data(), n, 4);
  double max_err = 0;
  for (uint32_t f = 0; f < n; ++f)
    for (uint32_t ch = 0; ch < 4; ++ch) {
      const float want = sa[f * 4 + ch] + (ch == 0 ? 0.125f : 0.f);
      max_err = std::max(max_err, double(std::fabs(got[f * 4 + ch] - want)));
    }
  std::printf("  loopback max error %.3g over %u frames (round trip %llu frames), %s\n", max_err,
              n, (unsigned long long)D, e.status_line().c_str());
  CHECK(max_err < 1e-6);

  // Nothing before the block (it was consumed, not replayed).
  std::vector<float> pre(48 * 4);
  a.read_rx(start + D - 48, pre.data(), 48, 4);
  for (float v : pre) CHECK(v == 0.f);

  // Meters saw the signal in both directions.
  std::vector<float> tx_peak, rx_peak;
  e.take_peaks(&tx_peak, &rx_peak);
  CHECK(tx_peak.size() == 4 && rx_peak.size() == 4);
  CHECK_NEAR(tx_peak[0], 0.375, 0.01);
  CHECK_NEAR(rx_peak[0], 0.375, 0.01);

  sim_run = false;
  sim.join();
  a.release_tx_slot();
  b.release_tx_slot();
  e.stop();
  CHECK(!a.daemon_alive());
}

TEST_MAIN()
