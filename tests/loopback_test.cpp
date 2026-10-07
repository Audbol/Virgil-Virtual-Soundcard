// Soundcard path end to end, with Inferno replaced by a network simulator:
// two playback clients -> mixer -> Dante TX ring --(sent at f + tx latency,
// written by the receiver at + rx latency)--> Dante RX ring -> capture ring
// -> client. Verifies mixing, 32-bit conversion and frame alignment.
#include <algorithm>
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
  // Generous receive latency: the simulator below is an ordinary thread that
  // a loaded CI machine may delay by several milliseconds.
  c.latency_us = 20000;
  c.tx_latency_us = 4000;
  c.ring_frames = 16384;

  Engine e(c);
  e.set_dante_enabled(false);
  CHECK(e.start());
  if (!e.running()) return;
  wait_ms(50);  // first ticks publish the clock
  // Inferno sends frame f at media time f + send delay, stamped f + tx
  // latency; the receiver writes it at stamp + its latency.
  const uint64_t L = c.us_to_frames(c.tx_send_delay_us()), RL = c.us_to_frames(c.latency_us);
  const uint64_t D = c.us_to_frames(c.tx_latency_us) + RL;
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
      // Inferno sends a frame L after its media time; the simulator forwards
      // as soon as the mixer is surely done with it (two ticks) so a slow CI
      // machine cannot make it late. Latency timing is covered by e2e_netns.sh.
      const uint64_t lag = std::min<uint64_t>(L, 2 * c.period_frames());
      const uint64_t sent_until = media > lag ? media - lag : 0;
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

  // The block comes back D frames later. Collect it as it becomes readable:
  // the capture ring only keeps half a ring (~170 ms) of history, and a busy
  // CI machine can oversleep a single long wait.
  std::vector<float> got(n * 4);
  // Readiness follows the engine's last finished tick, not the interpolated
  // clock, so a late tick cannot make us read a slot before it is filled.
  const int64_t give_up = mono_ns() + 5000000000LL;
  for (uint32_t have = 0; have < n && mono_ns() < give_up;) {
    const uint64_t done = a.header()->now_frames.load(std::memory_order_acquire);
    const uint64_t ready = done - 2 * a.period_frames();
    if (ready > start + D + have) {
      const uint32_t k = uint32_t(std::min<uint64_t>(ready - (start + D + have), n - have));
      a.read_rx(start + D + have, got.data() + size_t(have) * 4, k, 4);
      have += k;
    }
    wait_ms(2);
  }
  double max_err = 0;
  for (uint32_t f = 0; f < n; ++f)
    for (uint32_t ch = 0; ch < 4; ++ch) {
      const float want = sa[f * 4 + ch] + (ch == 0 ? 0.125f : 0.f);
      const double err = std::fabs(got[f * 4 + ch] - want);
      if (err > 1e-6 && max_err <= 1e-6)
        std::printf("  first mismatch at frame %u ch %u: got %g want %g\n", f, ch,
                    double(got[f * 4 + ch]), double(want));
      max_err = std::max(max_err, err);
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
