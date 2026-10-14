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
#include "virgil/config.h"
#include "virgil/engine.h"
#include "virgil/log.h"
#include "virgil/media_clock.h"
#include "virgil/platform.h"
#include "test_main.h"

using namespace virgil;

static void wait_ms(int ms) { sleep_until_ns(mono_ns() + int64_t(ms) * 1000000); }

// `C` channels each way (the shared memory layout has room for kMaxChannels),
// mixing every `tick_us`.
static void loopback(uint32_t C, uint32_t tick_us = 1000) {
  g_log_level = kLogWarn;
  Config c;
  c.device_name = "Loop Test";
  c.interface = "127.0.0.1";
#if defined(_WIN32)
  c.shm_name = std::string(kFallbackShmName) + "-test-" + std::to_string(process_id()) + "-" + std::to_string(C) +
               "-" + std::to_string(tick_us);
#else
  // macOS caps shared memory names at 31 characters: keep it short.
  c.shm_name = "/vgl-" + std::to_string(process_id()) + "-" + std::to_string(C) + "-" + std::to_string(tick_us);
#endif
  c.clock = "free";
  c.lock_memory = false;
  c.tx_channels = C;
  c.rx_channels = C;
  // Generous receive latency: the simulator below is an ordinary thread that
  // a loaded CI machine may delay by several milliseconds.
  c.latency_us = 20000;
  c.tx_latency_us = 4000;
  c.ring_frames = 16384;
  c.tick_us = tick_us;
  std::string err;
  CHECK(validate_config(&c, &err));

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
      if (!media) continue;  // no tick yet: 0 is not a media time
      if (done == 0) done = media - 64;
      // Inferno sends a frame L after its media time; the simulator forwards
      // as soon as the mixer is surely done with it (two ticks) so a slow CI
      // machine cannot make it late. Latency timing is covered by e2e_netns.sh.
      const uint64_t lag = std::min<uint64_t>(L, 2 * c.period_frames());
      const uint64_t sent_until = media > lag ? media - lag : 0;
      for (; done < sent_until; ++done)
        for (uint32_t ch = 0; ch < C; ++ch)
          rx[size_t((done + D) & dmask) * C + ch] = tx[size_t(done & dmask) * C + ch];
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

  // The engine realigns to the clock once just after it starts (a clock
  // step clears the rings, as audio from the old timeline must not go out).
  // On a slow machine that can come after a write: wait for it first.
  for (int64_t until = mono_ns() + 2000000000LL;
       a.header()->clock_steps.load() == 0 && mono_ns() < until;)
    wait_ms(5);
  wait_ms(20);
  const uint64_t steps_before = a.header()->clock_steps.load();

  // The audio is made before reading the clock: at 128 channels under
  // emulation (arm64 CI) making it could take longer than a short head start.
  const uint32_t n = 2400;
  std::vector<float> sa(n * C), sb(n * 2);
  for (uint32_t f = 0; f < n; ++f) {
    for (uint32_t ch = 0; ch < C; ++ch)
      sa[f * C + ch] = 0.25f * std::sin(0.01f * float(f) * float(ch + 1));
    sb[f * 2 + 0] = 0.125f;  // DC on ch 1 from client b
    sb[f * 2 + 1] = 0.f;
  }
  double now;
  CHECK(a.frame_now(&now));
  const uint64_t start = uint64_t(now) + 2400;  // 50 ms ahead
  CHECK(a.write_tx(start, sa.data(), n, C) == 0);
  CHECK(b.write_tx(start, sb.data(), n, 2) == 0);

  // The block comes back D frames later. Collect it as it becomes readable:
  // the capture ring only keeps half a ring (~170 ms) of history, and a busy
  // CI machine can oversleep a single long wait.
  std::vector<float> got(n * C);
  // Readiness follows the engine's last finished tick, not the interpolated
  // clock, so a late tick cannot make us read a slot before it is filled.
  const int64_t give_up = mono_ns() + 5000000000LL;
  for (uint32_t have = 0; have < n && mono_ns() < give_up;) {
    const uint64_t done = a.header()->now_frames.load(std::memory_order_acquire);
    const uint64_t ready = done - 2 * a.period_frames();
    if (ready > start + D + have) {
      const uint32_t k = uint32_t(std::min<uint64_t>(ready - (start + D + have), n - have));
      a.read_rx(start + D + have, got.data() + size_t(have) * C, k, C);
      have += k;
    }
    wait_ms(2);
  }
  double max_err = 0;
  for (uint32_t f = 0; f < n; ++f)
    for (uint32_t ch = 0; ch < C; ++ch) {
      const float want = sa[f * C + ch] + (ch == 0 ? 0.125f : 0.f);
      const double err = std::fabs(got[f * C + ch] - want);
      if (err > 1e-6 && max_err <= 1e-6)
        std::printf("  first mismatch at frame %u ch %u: got %g want %g\n", f, ch,
                    double(got[f * C + ch]), double(want));
      max_err = std::max(max_err, err);
    }
  std::printf("  loopback max error %.3g over %u frames (tick %u us, round trip %llu frames), %s\n", max_err,
              n, tick_us, (unsigned long long)D, e.status_line().c_str());
  if (a.header()->clock_steps.load() != steps_before)
    std::printf("  note: the clock stepped during the round trip\n");
  CHECK(max_err < 1e-6);

  // Nothing before the block (it was consumed, not replayed).
  std::vector<float> pre(48 * C);
  a.read_rx(start + D - 48, pre.data(), 48, C);
  for (float v : pre) CHECK(v == 0.f);

  // Meters saw the signal in both directions.
  std::vector<float> tx_peak, rx_peak;
  e.take_peaks(&tx_peak, &rx_peak);
  CHECK(tx_peak.size() == C && rx_peak.size() == C);
  CHECK_NEAR(tx_peak[0], 0.375, 0.01);
  CHECK_NEAR(rx_peak[0], 0.375, 0.01);

  sim_run = false;
  sim.join();
  a.release_tx_slot();
  b.release_tx_slot();
  e.stop();
  CHECK(!a.daemon_alive());
}

TEST(soundcard_loopback_through_dante_rings) { loopback(4); }
TEST(soundcard_loopback_128_channels) { loopback(kMaxChannels); }
// The 0.5 ms tick Virgil Control offers: 24-frame periods at 48 kHz.
TEST(soundcard_loopback_half_ms_tick) { loopback(8, 500); }
TEST(soundcard_loopback_half_ms_tick_128_channels) { loopback(kMaxChannels, 500); }

TEST_MAIN()
