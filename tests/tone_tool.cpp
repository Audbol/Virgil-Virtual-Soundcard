// virgil_tone: end-to-end test helper.
//
//   virgil_tone play SECONDS     channel 1: 1 kHz sine, channel 2: a click on
//                                every media frame that is a multiple of the rate
//   virgil_tone listen SECONDS   reports level and frequency on capture channel 1
//                                and where the clicks on channel 2 land
//
// Uses the daemon named by $VIRGIL_SHM_NAME. Because sender and receiver share
// the Dante media clock, the click offset is the end-to-end latency in frames.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

#include "virgil/client.h"
#include "virgil/platform.h"

using virgil::Client;

static const double kPi = 3.14159265358979323846;

static int play(Client& c, double seconds) {
  if (!c.acquire_tx_slot("tone")) return 1;
  c.set_tx_active(true);
  const uint32_t rate = c.sample_rate(), ch = c.tx_channels();
  const uint64_t lead = rate / 50;  // 20 ms ahead
  double now = 0;
  while (!c.frame_now(&now)) virgil::sleep_until_ns(virgil::mono_ns() + 5000000);
  uint64_t next = c.tx_horizon() + lead;
  const int64_t end = virgil::mono_ns() + int64_t(seconds * 1e9);
  std::vector<float> buf;
  while (virgil::mono_ns() < end) {
    c.frame_now(&now);
    const uint64_t until = uint64_t(now) + 2 * lead;
    if (until > next) {
      const uint32_t n = uint32_t(until - next);
      buf.assign(size_t(n) * ch, 0.f);
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t f = next + i;
        buf[size_t(i) * ch] = float(0.5 * std::sin(2 * kPi * 1000.0 * double(f % rate) / rate));
        if (ch > 1 && f % rate == 0) buf[size_t(i) * ch + 1] = 0.9f;
      }
      c.write_tx(next, buf.data(), n, ch);
      next = until;
    }
    c.touch();
    virgil::sleep_until_ns(virgil::mono_ns() + 5000000);
  }
  c.release_tx_slot();
  return 0;
}

static int listen(Client& c, double seconds) {
  const uint32_t rate = c.sample_rate(), ch = c.rx_channels();
  double now = 0;
  while (!c.frame_now(&now)) virgil::sleep_until_ns(virgil::mono_ns() + 5000000);
  // Capture is complete up to now - one tick and kept for half a ring.
  const uint64_t behind = 4 * c.period_frames();
  uint64_t pos = uint64_t(now) - behind;
  const int64_t end = virgil::mono_ns() + int64_t(seconds * 1e9);
  std::vector<float> buf(size_t(rate) * ch);
  int good = 0, seconds_seen = 0;
  while (virgil::mono_ns() < end) {
    // Collect one second in 20 ms pieces: the capture ring is shorter than a
    // second and the daemon clears it ahead of the write position.
    for (uint32_t got = 0; got < rate;) {
      virgil::sleep_until_ns(virgil::mono_ns() + 20000000LL);
      c.frame_now(&now);
      const uint64_t ready = uint64_t(now) - behind;
      if (ready <= pos + got) continue;
      const uint32_t n = uint32_t(std::min<uint64_t>(ready - (pos + got), rate - got));
      c.read_rx(pos + got, buf.data() + size_t(got) * ch, n, ch);
      got += n;
    }
    double sum = 0;
    int crossings = 0;
    long click = -1;
    for (uint32_t i = 0; i < rate; ++i) {
      const float s = buf[size_t(i) * ch];
      sum += double(s) * s;
      if (i && (buf[size_t(i - 1) * ch] < 0) != (s < 0)) ++crossings;
      if (ch > 1 && click < 0 && std::fabs(buf[size_t(i) * ch + 1]) > 0.5f)
        click = long((pos + i) % rate);
    }
    if (std::getenv("VIRGIL_TONE_DUMP") && seconds_seen == 0) {
      for (uint32_t i = 0; i < 48; ++i)
        std::printf("%llu: %+.4f %+.4f\n", (unsigned long long)(pos + i), buf[size_t(i) * ch],
                    ch > 1 ? buf[size_t(i) * ch + 1] : 0.f);
    }
    const double rms = std::sqrt(sum / rate);
    const double freq = crossings / 2.0;
    std::printf("ch1 rms %.3f freq %.0f Hz | click at media frame %% rate = %ld\n", rms, freq,
                click);
    std::fflush(stdout);
    ++seconds_seen;
    if (rms > 0.3 && std::fabs(freq - 1000) < 5) ++good;
    pos += rate;
  }
  std::printf("%d of %d seconds carried the tone\n", good, seconds_seen);
  return good > 0 ? 0 : 3;
}

int main(int argc, char** argv) {
  if (argc < 3 || (std::strcmp(argv[1], "play") && std::strcmp(argv[1], "listen"))) {
    std::fprintf(stderr, "usage: virgil_tone play|listen SECONDS\n");
    return 2;
  }
  Client c;
  if (!c.open() || !c.daemon_alive()) {
    std::fprintf(stderr, "virgild is not running\n");
    return 1;
  }
  const double s = std::atof(argv[2]);
  return !std::strcmp(argv[1], "play") ? play(c, s) : listen(c, s);
}
