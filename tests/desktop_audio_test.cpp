// Desktop audio engine (tools/control_common/desktop_audio_engine.cpp, the
// Linux / macOS one) against a fake sound system and an in-process Virgil
// engine: mixing, channel mapping, gain / mute / mono, the "Desktop audio"
// playback slot, receive channels to the output, app streams coming and
// going, following the default device, the feedback warning, reconnecting
// after Virgil restarts, and the settings file.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "desktop_audio_engine.h"
#include "test_main.h"
#include "virgil/client.h"
#include "virgil/engine.h"
#include "virgil/log.h"
#include "virgil/platform.h"

namespace {

void wait_ms(int ms) { virgil::sleep_until_ns(virgil::mono_ns() + int64_t(ms) * 1000000); }

// What a fake device or app stream records: DC per channel (DC passes the
// resampler unchanged, so levels can be checked exactly).
struct Signal {
  float l = 0, r = 0;
};

class FakeBackend;

class FakeStream : public desk::Stream {
 public:
  FakeStream(FakeBackend* be, bool capture, unsigned ch, unsigned rate, std::string device, uint64_t id,
             desk::AudioSink* sink, desk::AudioSource* src)
      : be_(be), capture_(capture), ch_(ch), rate_(rate), device_(std::move(device)), id_(id), sink_(sink), src_(src) {}
  ~FakeStream() override;
  bool start(std::string*) override;
  bool failed(std::string* why) const override {
    if (failed_.load() && why) *why = "fake failure";
    return failed_.load();
  }
  unsigned rate() const override { return rate_; }
  std::string device() const override { return device_; }
  void fail() { failed_ = true; }
  uint64_t id() const { return id_; }
  bool capture() const { return capture_; }

 private:
  void run();
  FakeBackend* be_;
  const bool capture_;
  const unsigned ch_, rate_;
  const std::string device_;
  const uint64_t id_;
  desk::AudioSink* sink_;
  desk::AudioSource* src_;
  std::atomic<bool> failed_{false}, quit_{false};
  std::thread th_;
};

class FakeBackend : public desk::Backend {
 public:
  std::mutex mu;
  bool available = true;
  std::string default_dev = "spk";
  std::map<std::string, Signal> devices = {{"spk", {0.25f, -0.125f}}, {"hp", {0.5f, 0.5f}}, {"usb", {0.f, 0.f}}};
  std::map<std::string, unsigned> device_rate = {{"spk", 44100}, {"hp", 48000}, {"usb", 48000}};
  struct AppS {
    uint64_t id;
    std::string app;
    float level;
  };
  std::vector<AppS> app_list = {{1, "Player", 0.3f}, {2, "Player", 0.1f}, {3, "Other", 0.7f}};
  std::vector<FakeStream*> live;      // guarded by mu
  std::vector<std::string> opened;    // "dev:<id>", "app:<id>", "out:<id>"
  std::vector<float> played;          // what the render stream got (left channel), last 2 s
  std::atomic<unsigned> changes_{0}, defaults_{0};

  std::string name() const override { return "Fake"; }
  bool connect(std::string* why) override {
    std::lock_guard<std::mutex> l(mu);
    if (!available && why) *why = "Desktop audio needs the fake sound system.";
    return available;
  }
  desk::Capabilities capabilities() override { return desk::Capabilities(); }
  std::vector<desk::Device> outputs() override {
    std::lock_guard<std::mutex> l(mu);
    std::vector<desk::Device> r;
    for (auto& kv : devices) r.push_back({kv.first, kv.first, kv.first == default_dev});
    return r;
  }
  std::string default_output() override {
    std::lock_guard<std::mutex> l(mu);
    return default_dev;
  }
  std::vector<desk::App> apps() override {
    std::lock_guard<std::mutex> l(mu);
    std::vector<desk::App> r;
    for (auto& a : app_list)
      if (std::none_of(r.begin(), r.end(), [&](const desk::App& x) { return x.id == a.app; }))
        r.push_back({a.app, a.app, true});
    return r;
  }
  std::vector<desk::AppStream> app_streams(const std::string& app, bool all) override {
    std::lock_guard<std::mutex> l(mu);
    std::vector<desk::AppStream> r;
    for (auto& a : app_list)
      if (all || a.app == app) r.push_back({a.id, a.app, 0});
    return r;
  }
  unsigned changes() override { return changes_.load(); }
  unsigned default_changes() override { return defaults_.load(); }

  std::unique_ptr<desk::Stream> open_device_capture(const std::string& device, unsigned ch, desk::AudioSink* sink,
                                                    std::string* error) override {
    std::lock_guard<std::mutex> l(mu);
    const std::string id = device.empty() || device == "default" ? default_dev : device;
    if (!devices.count(id)) {
      *error = "The device is not connected";
      return nullptr;
    }
    opened.push_back("dev:" + id);
    return std::unique_ptr<desk::Stream>(new FakeStream(this, true, ch, device_rate[id], id, 0, sink, nullptr));
  }
  std::unique_ptr<desk::Stream> open_app_capture(const desk::AppStream& s, unsigned ch, desk::AudioSink* sink,
                                                 std::string* error) override {
    std::lock_guard<std::mutex> l(mu);
    opened.push_back("app:" + std::to_string(s.id));
    return std::unique_ptr<desk::Stream>(new FakeStream(this, true, ch, 48000, default_dev, s.id, sink, nullptr));
  }
  std::unique_ptr<desk::Stream> open_render(const std::string& device, unsigned ch, double, desk::AudioSource* src,
                                            std::string*) override {
    std::lock_guard<std::mutex> l(mu);
    const std::string id = device.empty() || device == "default" ? default_dev : device;
    opened.push_back("out:" + id);
    return std::unique_ptr<desk::Stream>(new FakeStream(this, false, ch, 44100, id, 0, nullptr, src));
  }

  // What one capture stream records now.
  Signal signal_of(const FakeStream& s) {
    std::lock_guard<std::mutex> l(mu);
    if (s.id()) {
      for (auto& a : app_list)
        if (a.id == s.id()) return {a.level, a.level};
      return {};
    }
    return devices[s.device()];
  }
  void record(const float* d, size_t frames, unsigned ch) {
    std::lock_guard<std::mutex> l(mu);
    for (size_t f = 0; f < frames; ++f) played.push_back(d[f * ch]);
    if (played.size() > 88200) played.erase(played.begin(), played.end() - 88200);
  }
  size_t count(const std::string& what) {
    std::lock_guard<std::mutex> l(mu);
    return size_t(std::count(opened.begin(), opened.end(), what));
  }
  size_t live_count() {
    std::lock_guard<std::mutex> l(mu);
    return live.size();
  }
  void fail_app_stream(uint64_t id) {
    std::lock_guard<std::mutex> l(mu);
    app_list.erase(std::remove_if(app_list.begin(), app_list.end(), [&](const AppS& a) { return a.id == id; }),
                   app_list.end());
    for (FakeStream* s : live)
      if (s->id() == id) s->fail();
    changes_++;
  }
};

FakeStream::~FakeStream() {
  quit_ = true;
  if (th_.joinable()) th_.join();
  std::lock_guard<std::mutex> l(be_->mu);
  be_->live.erase(std::remove(be_->live.begin(), be_->live.end(), this), be_->live.end());
}

bool FakeStream::start(std::string*) {
  {
    std::lock_guard<std::mutex> l(be_->mu);
    be_->live.push_back(this);
  }
  th_ = std::thread([this] { run(); });
  return true;
}

// 10 ms packets on the system clock (a sound card exactly on time).
void FakeStream::run() {
  const size_t n = rate_ / 100;
  std::vector<float> buf(n * ch_);
  int64_t next = virgil::mono_ns();
  while (!quit_) {
    next += 10000000;
    virgil::sleep_until_ns(next);
    if (failed_) continue;
    if (capture_) {
      const Signal s = be_->signal_of(*this);
      for (size_t f = 0; f < n; ++f) {
        if (ch_ == 1) {
          buf[f] = (s.l + s.r) / 2;
        } else {
          buf[2 * f] = s.l;
          buf[2 * f + 1] = s.r;
        }
      }
      sink_->audio(buf.data(), n, next);
    } else {
      src_->render(buf.data(), n, next);
      be_->record(buf.data(), n, ch_);
    }
  }
}

struct Virgil {
  virgil::Config c;
  std::unique_ptr<virgil::Engine> e;
  std::atomic<bool> sim_run{false};
  std::thread sim;
  float rx_dc[4] = {-0.2f, 0.3f, 0.f, 0.f};

  explicit Virgil(const std::string& shm) {
    virgil::g_log_level = virgil::kLogWarn;
    c.device_name = "DA Test";
    c.interface = "127.0.0.1";
    c.shm_name = shm;
    c.clock = "free";
    c.lock_memory = false;
    c.tx_channels = 6;
    c.rx_channels = 4;
    c.latency_us = 10000;
    c.tx_latency_us = 4000;
    c.ring_frames = 16384;
  }
  ~Virgil() { stop(); }
  bool start() {
    e.reset(new virgil::Engine(c));
    e->set_dante_enabled(false);
    if (!e->start()) return false;
    // The Dante receive side: DC on the receive channels, written ahead.
    sim_run = true;
    sim = std::thread([this] {
      auto& rx = e->dante_rx_ring();
      const uint64_t dmask = c.ring_frames - 1;
      uint64_t done = 0;
      while (sim_run) {
        wait_ms(1);
        const uint64_t media = e->header()->now_frames.load();
        if (!done) done = media;
        for (; done < media + 2400; ++done)
          for (uint32_t ch = 0; ch < 4; ++ch)
            rx[size_t(done & dmask) * 4 + ch] = int32_t(std::lround(double(rx_dc[ch]) * 2147483647.0));
      }
    });
    return true;
  }
  void stop() {
    sim_run = false;
    if (sim.joinable()) sim.join();
    if (e) e->stop();
    e.reset();
  }
  // Mean of each transmit channel over the last 100 ms the engine mixed.
  std::vector<double> tx_means() {
    std::vector<double> m(6, 0.0);
    auto& tx = e->dante_tx_ring();
    const uint64_t dmask = c.ring_frames - 1;
    const uint64_t now = e->header()->now_frames.load();
    const uint64_t n = 4800, from = now - n - 96;
    for (uint64_t f = from; f < from + n; ++f)
      for (unsigned ch = 0; ch < 6; ++ch) m[ch] += double(tx[size_t(f & dmask) * 6 + ch]) / 2147483648.0;
    for (double& v : m) v /= double(n);
    return m;
  }
  int slot(const char* name) {
    for (unsigned i = 0; i < virgil::kMaxTxClients; ++i)
      if (e->header()->clients[i].pid.load() && virgil::slot_name(e->header()->clients[i]) == name) return int(i);
    return -1;
  }
};

// Short: macOS allows 31 characters.
std::string test_shm(const char* tag) {
#if defined(_WIN32)
  return std::string(virgil::kFallbackShmName) + "-d" + tag + std::to_string(virgil::process_id());
#else
  return std::string(virgil::kDefaultShmName) + "-d" + tag + std::to_string(virgil::process_id());
#endif
}

// Waits up to ms for cond.
template <class F>
bool eventually(int ms, F&& cond) {
  for (int64_t until = virgil::mono_ns() + int64_t(ms) * 1000000; virgil::mono_ns() < until; wait_ms(20))
    if (cond()) return true;
  return cond();
}

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

vc::DesktopAudioSettings base_settings() {
  vc::DesktopAudioSettings s;
  s.enabled = true;
  vc::DesktopAudioSource a;  // default device (spk, 44.1 kHz) -> 1 + 2
  a.type = vc::DesktopAudioSource::kDevice;
  a.device = "default";
  a.channel = 1;
  vc::DesktopAudioSource b;  // both of Player's streams, mixed to mono, -6 dB -> 3
  b.type = vc::DesktopAudioSource::kApp;
  b.app = "Player";
  b.channel = 3;
  b.mono = true;
  b.gain_db = -6;
  vc::DesktopAudioSource c;  // hp, muted -> 5 + 6
  c.type = vc::DesktopAudioSource::kDevice;
  c.device = "hp";
  c.channel = 5;
  c.mute = true;
  s.sources = {a, b, c};
  s.output.enabled = true;  // receive channel 2 on both sides of the output
  s.output.device = "usb";
  s.output.channel = 2;
  s.output.mono = true;
  return s;
}

}  // namespace

TEST(desktop_audio_mixes_maps_and_plays) {
  Virgil v(test_shm("m"));
  CHECK(v.start());
  if (!v.e) return;
  auto* fake = new FakeBackend;
  desk::Engine eng(std::unique_ptr<desk::Backend>(fake), v.c.shm_name);
  eng.start(base_settings());

  const double g6 = vc::db_to_gain(-6);
  auto tx_ok = [&](double c1, double c2, double c3) {
    const std::vector<double> m = v.tx_means();
    return near(m[0], c1, 0.004) && near(m[1], c2, 0.004) && near(m[2], c3, 0.004) && near(m[3], 0, 1e-6) &&
           near(m[4], 0, 1e-6) && near(m[5], 0, 1e-6);
  };
  const bool mixed = eventually(4000, [&] { return tx_ok(0.25, -0.125, 0.4 * g6); });
  const std::vector<double> m = v.tx_means();
  std::printf("  tx means: %.4f %.4f %.4f %.4f %.4f %.4f\n", m[0], m[1], m[2], m[3], m[4], m[5]);
  CHECK(mixed);
  CHECK(v.slot("Desktop audio") >= 0);

  desk::Status st;
  eventually(2000, [&] {
    st = eng.status();
    return st.sources.size() == 3 && st.sources[0].text.find("Capturing") == 0 &&
           st.sources[1].text.find("Capturing") == 0;
  });
  CHECK(st.phase == desk::Status::kRunning);
  CHECK(st.connected && st.tx == 6 && st.rx == 4 && st.rate == 48000);
  CHECK(st.sources.size() == 3);
  if (st.sources.size() == 3) {
    std::printf("  sources: \"%s\" / \"%s\" / \"%s\"\n", st.sources[0].text.c_str(), st.sources[1].text.c_str(),
                st.sources[2].text.c_str());
    CHECK(st.sources[0].text.find("Capturing (44.1 kHz") == 0);
    CHECK(st.sources[1].text.find("2 streams") != std::string::npos);
    CHECK(!st.sources[0].error && !st.sources[1].error && !st.sources[2].error);
    CHECK(st.sources[0].sound && st.sources[1].sound && !st.sources[2].sound);  // the muted one is silent
  }

  // Receive channel 2 (0.3) on both sides of the output device (usb, played at 44.1 kHz).
  auto played_ok = [&](double want) {
    std::lock_guard<std::mutex> l(fake->mu);
    if (fake->played.size() < 4410) return false;
    double sum = 0;
    for (size_t i = fake->played.size() - 4410; i < fake->played.size(); ++i) sum += fake->played[i];
    return near(sum / 4410, want, 0.004);
  };
  CHECK(eventually(3000, [&] { return played_ok(0.3); }));
  eventually(2000, [&] { return (st = eng.status()).output.text.find("Playing") == 0; });
  std::printf("  output: \"%s\"\n", st.output.text.c_str());
  CHECK(st.output.text.find("Playing (44.1 kHz") == 0 && !st.output.error);
  CHECK(st.output.sound);

  // Gain and mute change without reopening anything.
  const size_t opens = fake->count("dev:spk") + fake->count("app:1") + fake->count("out:usb");
  vc::DesktopAudioSettings s = base_settings();
  s.sources[0].gain_db = -6;
  s.sources[2].mute = false;
  s.output.gain_db = -6;
  eng.apply(s);
  CHECK(eventually(3000, [&] {
    const std::vector<double> t = v.tx_means();
    return near(t[0], 0.25 * g6, 0.004) && near(t[4], 0.5, 0.004) && near(t[5], 0.5, 0.004);
  }));
  CHECK(eventually(3000, [&] { return played_ok(0.3 * g6); }));
  CHECK(fake->count("dev:spk") + fake->count("app:1") + fake->count("out:usb") == opens);

  // One of Player's streams ends: the other one stays.
  fake->fail_app_stream(1);
  CHECK(eventually(3000, [&] { return near(v.tx_means()[2], 0.1 * g6, 0.004); }));
  CHECK(eventually(2000, [&] {
    st = eng.status();
    return st.sources.size() == 3 && st.sources[1].text.find("Capturing") == 0 &&
           st.sources[1].text.find("streams") == std::string::npos;
  }));

  // The default output changes: the "default" source follows it.
  {
    std::lock_guard<std::mutex> l(fake->mu);
    fake->default_dev = "hp";
  }
  fake->defaults_++;
  CHECK(eventually(3000, [&] { return fake->count("dev:hp") >= 2; }));  // the hp source + the default one
  CHECK(eventually(3000, [&] { return near(v.tx_means()[0], 0.5 * g6, 0.004); }));

  // The output on a device that is also captured: a warning.
  CHECK(!eng.status().output.error);
  s.output.device = "hp";
  eng.apply(s);
  CHECK(eventually(3000, [&] {
    const desk::Status t = eng.status();
    return t.output.error && t.output.text.find("echo") != std::string::npos;
  }));

  // A new route restarts the session; a playback slot is still held once.
  s.sources.pop_back();
  eng.apply(s);
  CHECK(eventually(3000, [&] { return near(v.tx_means()[4], 0, 1e-6) && near(v.tx_means()[0], 0.5 * g6, 0.004); }));
  int held = 0;
  for (unsigned i = 0; i < virgil::kMaxTxClients; ++i)
    if (v.e->header()->clients[i].pid.load()) ++held;
  CHECK(held == 1);

  // Off: the slot and every stream go away.
  s.enabled = false;
  eng.apply(s);
  CHECK(eventually(3000, [&] { return v.slot("Desktop audio") < 0 && fake->live_count() == 0; }));
  CHECK(eventually(1000, [&] { return eng.status().phase == desk::Status::kOff; }));
  eng.shutdown();
}

TEST(desktop_audio_waits_for_virgil_and_reconnects) {
  const std::string shm = test_shm("r");
  auto* fake = new FakeBackend;
  desk::Engine eng(std::unique_ptr<desk::Backend>(fake), shm);
  vc::DesktopAudioSettings s = base_settings();
  s.sources.resize(1);
  s.output.enabled = false;
  eng.start(s);
  CHECK(eventually(2000, [&] { return eng.status().phase == desk::Status::kWaiting; }));
  std::printf("  without Virgil: \"%s\"\n", eng.status().text.c_str());
  CHECK(fake->live_count() == 0);

  {
    Virgil v(shm);
    CHECK(v.start());
    CHECK(eventually(5000, [&] { return near(v.tx_means()[0], 0.25, 0.004); }));
    CHECK(eng.status().phase == desk::Status::kRunning);
    v.stop();
  }
  // Virgil gone: the session ends, desktop audio waits again.
  CHECK(eventually(5000, [&] { return eng.status().phase == desk::Status::kWaiting && fake->live_count() == 0; }));
  std::printf("  after Virgil stopped: \"%s\"\n", eng.status().text.c_str());

  // Back (a new soundcard, as after a restart with another layout).
  Virgil v2(shm);
  v2.c.tx_channels = 6;
  CHECK(v2.start());
  CHECK(eventually(6000, [&] { return near(v2.tx_means()[0], 0.25, 0.004); }));
  CHECK(v2.slot("Desktop audio") >= 0);
  eng.shutdown();
  CHECK(v2.slot("Desktop audio") < 0);  // released at shutdown
}

TEST(desktop_audio_without_sound_system) {
  Virgil v(test_shm("n"));
  CHECK(v.start());
  auto* fake = new FakeBackend;
  fake->available = false;
  desk::Engine eng(std::unique_ptr<desk::Backend>(fake), v.c.shm_name);
  eng.start(base_settings());
  CHECK(eventually(2000, [&] { return eng.status().phase == desk::Status::kError; }));
  const desk::Status st = eng.status();
  std::printf("  \"%s\"\n", st.text.c_str());
  CHECK(st.text.find("fake sound system") != std::string::npos);
  CHECK(v.slot("Desktop audio") < 0);  // no playback slot taken for nothing
  {
    std::lock_guard<std::mutex> l(fake->mu);
    fake->available = true;
  }
  CHECK(eventually(6000, [&] { return eng.status().phase == desk::Status::kRunning; }));
  eng.shutdown();
}

TEST(desktop_audio_bad_channels_are_reported) {
  Virgil v(test_shm("c"));
  CHECK(v.start());
  auto* fake = new FakeBackend;
  desk::Engine eng(std::unique_ptr<desk::Backend>(fake), v.c.shm_name);
  vc::DesktopAudioSettings s = base_settings();
  s.sources.resize(1);
  s.sources[0].channel = 9;  // Virgil has 6
  s.output.channel = 7;      // and 4 receive channels
  eng.start(s);
  CHECK(eventually(3000, [&] {
    const desk::Status t = eng.status();
    return t.sources.size() == 1 && t.sources[0].error && t.output.error;
  }));
  const desk::Status t = eng.status();
  if (t.sources.size() == 1) {
    std::printf("  \"%s\" / \"%s\"\n", t.sources[0].text.c_str(), t.output.text.c_str());
    CHECK(t.sources[0].text.find("6 transmit channels") != std::string::npos);
    CHECK(t.output.text.find("4 receive channels") != std::string::npos);
  }
  eng.shutdown();
}

TEST(desktop_audio_settings_file) {
#if !defined(_WIN32)
  char tmpl[] = "/tmp/virgil-da-test-XXXXXX";
  const char* dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (!dir) return;
  setenv("XDG_CONFIG_HOME", dir, 1);
#if defined(__linux__)
  CHECK(desk::settings_path() == std::string(dir) + "/virgil/desktop-audio.conf");
#endif
  const std::string path = std::string(dir) + "/virgil/desktop-audio.conf";
  std::string err;
  vc::DesktopAudioSettings none = desk::load_settings(path, &err);
  CHECK(err.empty() && !none.enabled && none.sources.empty());  // missing: defaults, no error

  vc::DesktopAudioSettings s = base_settings();
  s.sources[1].app = "Google Chrome";  // names with spaces (Linux app names)
  s.sources[0].device = "alsa_output.pci-0000_00_1f.3.analog-stereo";
  s.sources[0].name = "Built-in Audio Analog Stereo";
  CHECK(desk::save_settings(path, s, &err));  // creates the folder
  const vc::DesktopAudioSettings back = desk::load_settings(path, &err);
  CHECK(err.empty());
  CHECK(back == s);
  // Saved again over the existing file.
  s.enabled = false;
  CHECK(desk::save_settings(path, s, &err));
  CHECK(desk::load_settings(path, nullptr) == s);
  std::remove(path.c_str());
  std::remove((std::string(dir) + "/virgil").c_str());
  std::remove(dir);
  unsetenv("XDG_CONFIG_HOME");
#endif
}

TEST_MAIN()
