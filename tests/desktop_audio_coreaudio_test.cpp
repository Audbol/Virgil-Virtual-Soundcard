// macOS: the Core Audio desktop audio backend against the real Core Audio of
// the machine it runs on (CI runners may have no output device at all, so
// nothing here needs one). Never creates a process tap: that would ask for
// the audio recording permission.
#include <sys/sysctl.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "desktop_audio_engine.h"
#include "test_main.h"
#include "virgil/platform.h"

namespace {

// macOS version as major * 100 + minor (1402 = 14.2); 0 if unknown.
int os_version() {
  char buf[64] = {};
  size_t n = sizeof buf - 1;
  if (sysctlbyname("kern.osproductversion", buf, &n, nullptr, 0) != 0) return 0;
  int major = 0, minor = 0;
  if (std::sscanf(buf, "%d.%d", &major, &minor) < 1) return 0;
  return major * 100 + minor;
}

struct Silence : desk::AudioSource {
  std::atomic<unsigned> calls{0};
  void render(float* out, size_t frames, int64_t) override {
    for (size_t i = 0; i < frames * 2; ++i) out[i] = 0.f;
    calls.fetch_add(1);
  }
};

}  // namespace

TEST(coreaudio_backend_lists_and_reports) {
  std::unique_ptr<desk::Backend> be = desk::make_system_backend();
  CHECK(be != nullptr);
  if (!be) return;
  CHECK(!be->name().empty());
  std::string why;
  CHECK(be->connect(&why));

  const int v = os_version();
  const desk::Capabilities caps = be->capabilities();
  std::printf("  macOS %d.%d, app capture %s\n", v / 100, v % 100, caps.app_capture ? "yes" : "no");
  if (v >= 1402) {
    CHECK(caps.app_capture);
    CHECK(caps.all_apps);
  } else if (v > 0) {
    CHECK(!caps.app_capture);
    CHECK(!caps.all_apps);
    CHECK(!caps.app_hint.empty());
  }

  const std::vector<desk::Device> outs = be->outputs();
  int defaults = 0;
  for (const auto& d : outs) {
    CHECK(!d.id.empty());
    CHECK(!d.name.empty());
    CHECK(d.id.rfind("org.virgil.control.desktop-audio.", 0) != 0);  // never our own aggregates
    if (d.is_default) ++defaults;
    std::printf("  output: %s (%s)%s\n", d.name.c_str(), d.id.c_str(), d.is_default ? " [default]" : "");
  }
  CHECK(defaults <= 1);
  const std::string def = be->default_output();
  if (!def.empty() && defaults == 1) {
    bool found = false;
    for (const auto& d : outs) found = found || (d.is_default && d.id == def);
    CHECK(found);
  }

  // Process lists work without any permission.
  for (const auto& a : be->apps()) {
    CHECK(!a.id.empty());
    std::printf("  playing: %s (%s)\n", a.name.c_str(), a.id.c_str());
  }
  (void)be->app_streams("no such app, surely", false);
  CHECK(be->app_streams("", false).empty());
  (void)be->changes();
  (void)be->default_changes();

  // Unknown devices are refused with a reason.
  Silence src;
  std::string err;
  CHECK(be->open_render("no-such-device-uid", 2, 0.03, &src, &err) == nullptr);
  CHECK(!err.empty());
}

TEST(coreaudio_render_opens_and_closes) {
  std::unique_ptr<desk::Backend> be = desk::make_system_backend();
  if (!be || be->default_output().empty()) {
    std::printf("  no default output device: skipped\n");
    return;
  }
  Silence src;
  std::string err;
  std::unique_ptr<desk::Stream> s = be->open_render("default", 2, 0.03, &src, &err);
  if (!s) {
    std::printf("  cannot open the default output (%s): skipped\n", err.c_str());
    return;
  }
  CHECK(s->rate() >= 8000);
  CHECK(!s->device().empty());
  if (s->start(&err)) {
    virgil::sleep_until_ns(virgil::mono_ns() + 300000000LL);
    std::printf("  %u render calls in 0.3 s at %u Hz\n", src.calls.load(), s->rate());
  } else {
    std::printf("  start failed (%s)\n", err.c_str());
  }
  s.reset();  // stops it: no callback after this
  const unsigned after = src.calls.load();
  virgil::sleep_until_ns(virgil::mono_ns() + 100000000LL);
  CHECK(src.calls.load() == after);
}

TEST_MAIN()
