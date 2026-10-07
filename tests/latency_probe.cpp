// dsv_latency_probe: measure round-trip latency through the network.
// Plays a click on a playback channel and looks for it on a capture channel.
// Route the DSV transmit flow back to its receive flow (e.g. through a Dante
// device in Dante Controller) before running.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dsv/client.h"
#include "dsv/platform.h"

int main(int argc, char** argv) {
  const uint32_t out_ch = argc > 1 ? uint32_t(std::atoi(argv[1])) : 1;
  const uint32_t in_ch = argc > 2 ? uint32_t(std::atoi(argv[2])) : 1;
  dsv::Client c;
  if (!c.open() || !c.daemon_alive()) {
    std::fprintf(stderr, "dsvd is not running\n");
    return 1;
  }
  if (out_ch < 1 || out_ch > c.tx_channels() || in_ch < 1 || in_ch > c.rx_channels()) {
    std::fprintf(stderr, "usage: dsv_latency_probe [out_ch 1..%u] [in_ch 1..%u]\n",
                 c.tx_channels(), c.rx_channels());
    return 2;
  }
  if (!c.acquire_tx_slot("latency-probe")) return 1;
  c.set_tx_active(true);
  const uint32_t rate = c.sample_rate();
  std::vector<float> click(c.tx_channels(), 0.f);
  std::vector<float> frame(c.rx_channels());
  for (int i = 0; i < 5; ++i) {
    double now;
    c.frame_now(&now);
    const uint64_t at = uint64_t(now) + rate / 10;
    click[out_ch - 1] = 0.9f;
    c.write_tx(at, click.data(), 1, c.tx_channels());
    dsv::sleep_until_ns(dsv::mono_ns() + 600000000LL);
    c.touch();
    long found = -1;
    for (uint64_t f = at; f < at + rate / 2; ++f) {
      c.read_rx(f, frame.data(), 1, c.rx_channels());
      if (std::fabs(frame[in_ch - 1]) > 0.5f) {
        found = long(f - at);
        break;
      }
    }
    if (found < 0)
      std::printf("click %d: not received (check routing)\n", i + 1);
    else
      std::printf("click %d: network round trip %ld frames = %.3f ms\n", i + 1, found,
                  1000.0 * found / rate);
  }
  c.release_tx_slot();
  return 0;
}
