// End-to-end: client playback -> mixer -> AES67 RTP over UDP (localhost) ->
// receiver -> capture ring -> client capture. Verifies sample accuracy and
// that two clients are mixed.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dsv/client.h"
#include "dsv/engine.h"
#include "dsv/log.h"
#include "dsv/platform.h"
#include "test_main.h"

using namespace dsv;

static void wait_ms(int ms) { sleep_until_ns(mono_ns() + int64_t(ms) * 1000000); }

TEST(loopback_unicast) {
  g_log_level = kLogWarn;
  Config c;
  c.device_name = "DSV Loopback Test";
  c.interface = "127.0.0.1";
#if defined(_WIN32)
  c.shm_name = std::string(kFallbackShmName) + "-test-" + std::to_string(process_id());
#else
  c.shm_name = std::string(kDefaultShmName) + "-test-" + std::to_string(process_id());
#endif
  c.clock = "free";
  c.sap = false;
  c.lock_memory = false;
  c.tx_channels = 4;
  c.rx_channels = 4;
  c.packet_time_us = 250;
  c.rx_latency_us = 3000;
  c.ring_frames = 16384;  // block + latency must fit in half a ring
  StreamConfig tx;
  tx.address = "127.0.0.1";
  tx.port = 25004;
  tx.channels = 4;
  c.tx.push_back(tx);
  StreamConfig rx;
  rx.port = 25004;
  rx.channels = 4;
  c.rx.push_back(rx);

  Engine e(c);
  CHECK(e.start());
  if (!e.running()) return;
  wait_ms(50);

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
  CHECK(480 + n + a.rx_latency_frames() < a.ring_frames() / 2);
  std::vector<float> sa(n * 4), sb(n * 2);
  for (uint32_t f = 0; f < n; ++f) {
    for (uint32_t ch = 0; ch < 4; ++ch)
      sa[f * 4 + ch] = 0.25f * std::sin(0.01f * float(f) * float(ch + 1));
    sb[f * 2 + 0] = 0.125f;  // DC on ch 1 from client b
    sb[f * 2 + 1] = 0.f;
  }
  a.write_tx(start, sa.data(), n, 4);
  b.write_tx(start, sb.data(), n, 2);

  // Wait until the whole block is past the capture playout point.
  for (;;) {
    double t;
    a.frame_now(&t);
    if (t > double(start + n + a.rx_latency_frames() + a.period_frames())) break;
    wait_ms(5);
  }
  std::vector<float> got(n * 4);
  a.read_rx(start, got.data(), n, 4);
  double max_err = 0;
  for (uint32_t f = 0; f < n; ++f)
    for (uint32_t ch = 0; ch < 4; ++ch) {
      const float want = sa[f * 4 + ch] + (ch == 0 ? 0.125f : 0.f);
      max_err = std::max(max_err, double(std::fabs(got[f * 4 + ch] - want)));
    }
  std::printf("  loopback max error %.3g over %u frames, %s\n", max_err, n,
              e.status_line().c_str());
  CHECK(max_err < 1e-6);

  // Frames before the block must be silent (nothing leaked from the past).
  std::vector<float> pre(48 * 4);
  a.read_rx(start - 48, pre.data(), 48, 4);
  for (float v : pre) CHECK(v == 0.f);

  a.release_tx_slot();
  b.release_tx_slot();
  CHECK(!a.has_tx_slot());
  e.stop();
  CHECK(!a.daemon_alive());
}

TEST_MAIN()
