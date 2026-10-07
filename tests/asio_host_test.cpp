// Minimal ASIO host: loads VirgilAsio.dll directly (no registry), plays a sine
// on output 1 and records input 1. With virgild looping tx -> rx (see
// tests/loopback.conf) the recording must contain the sine without glitches.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

static const double kPi = 3.14159265358979323846;

static const CLSID kClsid = {0xacddf2ef, 0xfa26, 0x401c,
                             {0x9c, 0xa6, 0xba, 0x44, 0x7c, 0xea, 0x8d, 0x38}};

static ASIOBufferInfo g_bufs[2];
static long g_size = 0;
static double g_phase = 0;
static std::vector<int32_t> g_rec;
static long g_switches = 0;

static ASIOTime* on_switch_ti(ASIOTime* t, long idx, ASIOBool) {
  auto* out = static_cast<int32_t*>(g_bufs[0].buffers[idx]);
  auto* in = static_cast<int32_t*>(g_bufs[1].buffers[idx]);
  for (long i = 0; i < g_size; ++i) {
    out[i] = int32_t(0.5 * 2147483647.0 * std::sin(g_phase));
    g_phase += 2 * kPi * 1000.0 / 48000.0;
  }
  g_rec.insert(g_rec.end(), in, in + g_size);
  ++g_switches;
  return t;
}
static void on_switch(long idx, ASIOBool d) { on_switch_ti(nullptr, idx, d); }
static void on_rate(ASIOSampleRate) {}
static long on_message(long sel, long value, void*, double*) {
  if (sel == kAsioSelectorSupported) return value == kAsioSupportsTimeInfo;
  if (sel == kAsioSupportsTimeInfo) return 1;
  return 0;
}

int main(int argc, char** argv) {
  const char* dll = argc > 1 ? argv[1] : "VirgilAsio.dll";
  HMODULE m = LoadLibraryA(dll);
  if (!m) return std::printf("cannot load %s\n", dll), 1;
  auto get = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, void**)>(
      reinterpret_cast<void*>(GetProcAddress(m, "DllGetClassObject")));
  IClassFactory* f = nullptr;
  if (!get || get(kClsid, IID_IClassFactory, reinterpret_cast<void**>(&f)) != S_OK)
    return std::printf("no class factory\n"), 1;
  IASIO* a = nullptr;
  if (f->CreateInstance(nullptr, kClsid, reinterpret_cast<void**>(&a)) != S_OK)
    return std::printf("CreateInstance failed\n"), 1;
  if (!a->init(nullptr)) {
    char msg[128];
    a->getErrorMessage(msg);
    return std::printf("init failed: %s\n", msg), 1;
  }
  long ni, no, mn, mx, pref, gran, li, lo;
  ASIOSampleRate sr;
  a->getChannels(&ni, &no);
  a->getBufferSize(&mn, &mx, &pref, &gran);
  a->getSampleRate(&sr);
  g_size = pref;
  g_bufs[0] = {ASIOFalse, 0, {nullptr, nullptr}};
  g_bufs[1] = {ASIOTrue, 0, {nullptr, nullptr}};
  ASIOCallbacks cb{on_switch, on_rate, on_message, on_switch_ti};
  if (a->createBuffers(g_bufs, 2, g_size, &cb) != ASE_OK) return std::printf("createBuffers\n"), 1;
  a->getLatencies(&li, &lo);
  std::printf("channels %ld in / %ld out, %.0f Hz, buffer %ld (min %ld max %ld), latency in %ld "
              "out %ld frames\n", ni, no, sr, g_size, mn, mx, li, lo);
  a->start();
  Sleep(2000);
  a->stop();
  a->disposeBuffers();
  a->Release();

  // Find the sine in the recording and check it is continuous.
  size_t first = 0;
  while (first < g_rec.size() && std::abs(g_rec[first]) < (1 << 24)) ++first;
  const double c = 2 * std::cos(2 * kPi * 1000.0 / 48000.0);
  long bad = 0;
  size_t n = 0;
  for (size_t i = first + 8; i + 8 < g_rec.size(); ++i, ++n) {
    const double y = g_rec[i] / 2147483648.0, y1 = g_rec[i - 1] / 2147483648.0,
                 y2 = g_rec[i - 2] / 2147483648.0;
    if (std::fabs(y - (c * y1 - y2)) > 0.01) {
      if (bad < 12) std::printf("  glitch at frame %zu (buffer offset %zu)\n", i, i % size_t(g_size));
      ++bad;
    }
  }
  std::printf("buffer switches %ld, recorded %zu frames, sine from frame %zu: %zu frames checked, "
              "%ld discontinuities\n", g_switches, g_rec.size(), first, n, bad);
  return (n > 48000 && bad == 0) ? 0 : 1;
}
