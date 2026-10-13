// Desktop audio backend for Linux: the PulseAudio client API, which PipeWire
// serves as well (pipewire-pulse, the default on current Ubuntu, Mint, Fedora
// and Debian desktops). libpulse is loaded at run time, so Virgil Control
// starts and works without it (desktop audio then says what it needs).
//
//   output device   record from the sink's monitor source ("<sink>.monitor")
//   one app         record each of its sink inputs (pa_stream_set_monitor_stream),
//                   matched by application.name or application.process.binary
//   all apps        the same for every sink input except this process's own
//   output          a playback stream to a sink (or the default one)
//
// One threaded mainloop and one context for everything. A cache of sinks,
// sink inputs and the default sink is kept up to date by subscription events
// on the mainloop thread, so the queries the engine and the window make never
// wait for the server. Streams ask for float32 at the sink's own rate; the
// engine's AdaptiveBridge converts to Virgil's rate and follows the drift.
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "desktop_audio_engine.h"
#include "pulse_min.h"
#include "virgil/platform.h"

namespace desk {

namespace {

const char* const kNeeds = "Desktop audio needs PulseAudio or PipeWire (with pipewire-pulse)";

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

struct SinkRec {
  uint32_t index = 0;
  std::string name, desc, monitor;
  unsigned rate = 48000, channels = 2;
};
struct InputRec {
  uint32_t index = 0, sink = 0, pid = 0;
  std::string binary, app_name, media;
  bool corked = false;
  // An app is known by its name (application.name, "Firefox"), else by its
  // program (application.process.binary): several programs share binaries
  // (python3, pacat for paplay) and some name each stream differently.
  std::string key() const { return !app_name.empty() ? app_name : !binary.empty() ? binary : media; }
  std::string label() const { return !app_name.empty() ? app_name : !binary.empty() ? binary : media; }
};

class PulseBackend;

class PulseStream : public Stream {
 public:
  PulseStream(PulseBackend* be, bool capture, unsigned channels, unsigned rate, const std::string& pa_device,
              const std::string& device, uint32_t monitor_input, AudioSink* sink, AudioSource* src)
      : be_(be), capture_(capture), ch_(channels), rate_(rate), pa_device_(pa_device), device_(device),
        monitor_input_(monitor_input), sink_(sink), src_(src) {}
  ~PulseStream() override;

  bool create(pa_context* ctx, std::string* error);  // under the mainloop lock
  bool start(std::string* error) override;
  bool failed(std::string* why) const override {
    if (!failed_.load(std::memory_order_acquire)) return false;
    if (why) {
      std::lock_guard<std::mutex> l(mu_);
      *why = why_;
    }
    return true;
  }
  unsigned rate() const override { return rate_; }
  std::string device() const override { return device_; }

 private:
  static void state_cb(pa_stream* s, void* u);
  static void read_cb(pa_stream* s, size_t n, void* u);
  static void write_cb(pa_stream* s, size_t n, void* u);
  void feed(const uint8_t* p, size_t n, int64_t now);
  void fail(const std::string& why) {
    {
      std::lock_guard<std::mutex> l(mu_);
      if (why_.empty()) why_ = why;
    }
    failed_.store(true, std::memory_order_release);
  }

  PulseBackend* be_;
  const bool capture_;
  const unsigned ch_, rate_;
  const std::string pa_device_, device_;
  const uint32_t monitor_input_;
  AudioSink* const sink_;
  AudioSource* const src_;
  pa_context* ctx_ = nullptr;
  pa_stream* s_ = nullptr;
  std::atomic<bool> failed_{false};
  mutable std::mutex mu_;
  std::string why_;
  // Mainloop thread only.
  uint8_t carry_[4 * 8] = {};
  size_t carry_n_ = 0;
  std::vector<float> buf_;
};

class PulseBackend : public Backend {
 public:
  PulseBackend() : self_pid_(uint32_t(getpid())) {}
  ~PulseBackend() override {
    if (ml_) {
      pa_.threaded_mainloop_lock(ml_);
      drop_context();
      pa_.threaded_mainloop_unlock(ml_);
      pa_.threaded_mainloop_stop(ml_);
      pa_.threaded_mainloop_free(ml_);
    }
    // libpulse stays loaded: it may have left atexit handlers.
  }

  std::string name() const override { return "PulseAudio"; }

  bool connect(std::string* why) override {
    std::lock_guard<std::mutex> g(connect_mu_);
    if (!load(why)) return false;
    if (!ml_) {
      ml_ = pa_.threaded_mainloop_new();
      if (!ml_ || pa_.threaded_mainloop_start(ml_) < 0) {
        if (ml_) pa_.threaded_mainloop_free(ml_);
        ml_ = nullptr;
        if (why) *why = std::string(kNeeds) + ": cannot start its client thread.";
        return false;
      }
    }
    bool need = false;
    {
      std::lock_guard<std::mutex> l(mu_);
      if (state_ == PA_CONTEXT_READY_ && synced_) return true;
      need = state_ < 0 || state_ == PA_CONTEXT_FAILED_ || state_ == PA_CONTEXT_TERMINATED_;
    }
    if (need) {
      const int64_t now = virgil::mono_ns();
      if (now < next_try_) {
        if (why) *why = not_running_text();
        return false;
      }
      next_try_ = now + 2000000000LL;
      pa_.threaded_mainloop_lock(ml_);
      drop_context();
      pa_proplist* p = pa_.proplist_new();
      pa_.proplist_sets(p, "application.name", "Virgil Control");
      pa_.proplist_sets(p, "application.id", "org.virgil.Control");
      pa_.proplist_sets(p, "application.icon_name", "virgil");
      ctx_ = pa_.context_new_with_proplist(pa_.threaded_mainloop_get_api(ml_), "Virgil Control", p);
      pa_.proplist_free(p);
      {
        std::lock_guard<std::mutex> l(mu_);
        state_ = PA_CONTEXT_CONNECTING_;
        synced_ = false;
        error_.clear();
      }
      if (!ctx_) {
        std::lock_guard<std::mutex> l(mu_);
        state_ = PA_CONTEXT_FAILED_;
        error_ = "out of memory";
      } else {
        pa_.context_set_state_callback(ctx_, &PulseBackend::context_state_cb, this);
        pa_.context_set_subscribe_callback(ctx_, &PulseBackend::subscribe_cb, this);
        if (pa_.context_connect(ctx_, nullptr, PA_CONTEXT_NOAUTOSPAWN_, nullptr) < 0) {
          std::lock_guard<std::mutex> l(mu_);
          state_ = PA_CONTEXT_FAILED_;
          error_ = pa_.strerror(pa_.context_errno(ctx_));
        }
      }
      pa_.threaded_mainloop_unlock(ml_);
    }
    std::unique_lock<std::mutex> l(mu_);
    cv_.wait_for(l, std::chrono::seconds(3), [this] {
      return (state_ == PA_CONTEXT_READY_ && synced_) || state_ == PA_CONTEXT_FAILED_ ||
             state_ == PA_CONTEXT_TERMINATED_;
    });
    if (state_ == PA_CONTEXT_READY_ && synced_) return true;
    if (why) *why = not_running_text_locked();
    return false;
  }

  Capabilities capabilities() override { return Capabilities(); }

  std::vector<Device> outputs() override {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<Device> r;
    for (const auto& kv : sinks_) {
      Device d;
      d.id = kv.second.name;
      d.name = kv.second.desc.empty() ? kv.second.name : kv.second.desc;
      d.is_default = kv.second.name == default_sink_;
      r.push_back(d);
    }
    std::sort(r.begin(), r.end(), [](const Device& a, const Device& b) { return a.name < b.name; });
    return r;
  }

  std::string default_output() override {
    std::lock_guard<std::mutex> l(mu_);
    return default_sink_;
  }

  std::vector<App> apps() override {
    std::lock_guard<std::mutex> l(mu_);
    std::map<std::string, App> by_key;
    for (const auto& kv : inputs_) {
      const InputRec& in = kv.second;
      if (in.pid == self_pid_ || in.key().empty()) continue;
      App& a = by_key[lower(in.key())];
      if (a.id.empty()) {
        a.id = in.key();
        a.name = in.label();
      }
      a.playing |= !in.corked;
    }
    std::vector<App> r;
    for (auto& kv : by_key) r.push_back(kv.second);
    std::sort(r.begin(), r.end(), [](const App& a, const App& b) { return lower(a.name) < lower(b.name); });
    return r;
  }

  std::vector<AppStream> app_streams(const std::string& app, bool all) override {
    std::string want = lower(app);
    const size_t slash = want.find_last_of('/');
    if (slash != std::string::npos) want = want.substr(slash + 1);  // a full path typed in
    std::lock_guard<std::mutex> l(mu_);
    std::vector<AppStream> r;
    for (const auto& kv : inputs_) {
      const InputRec& in = kv.second;
      if (in.pid == self_pid_) continue;
      if (all) {
        // Not what plays on a Virgil sink, if the user made one (that would
        // send Virgil's own playback back into it).
        auto s = sinks_.find(in.sink);
        if (s != sinks_.end() && lower(s->second.name).find("virgil") != std::string::npos) continue;
      } else if (want.empty() || (lower(in.binary) != want && lower(in.app_name) != want)) {
        continue;
      }
      AppStream a;
      a.id = in.index;
      a.app = in.key();
      a.pid = in.pid;
      r.push_back(a);
    }
    return r;
  }

  unsigned changes() override { return changes_.load(std::memory_order_acquire); }
  unsigned default_changes() override { return defaults_.load(std::memory_order_acquire); }

  std::unique_ptr<Stream> open_device_capture(const std::string& device, unsigned channels, AudioSink* sink,
                                              std::string* error) override {
    if (!connect(error)) return nullptr;
    SinkRec rec;
    if (!find_sink(device, &rec, error)) return nullptr;
    return make(true, channels, rec.rate, rec.monitor, rec.name, PA_INVALID_INDEX_, sink, nullptr, error);
  }

  std::unique_ptr<Stream> open_app_capture(const AppStream& s, unsigned channels, AudioSink* sink,
                                           std::string* error) override {
    if (!connect(error)) return nullptr;
    SinkRec rec;
    {
      std::lock_guard<std::mutex> l(mu_);
      auto in = inputs_.find(uint32_t(s.id));
      if (in == inputs_.end()) {
        if (error) *error = "The app stopped playing";
        return nullptr;
      }
      auto sk = sinks_.find(in->second.sink);
      if (sk == sinks_.end()) {
        if (error) *error = "The app plays on an unknown device";
        return nullptr;
      }
      rec = sk->second;
    }
    return make(true, channels, rec.rate, rec.monitor, rec.name, uint32_t(s.id), sink, nullptr, error);
  }

  std::unique_ptr<Stream> open_render(const std::string& device, unsigned channels, double latency_s,
                                      AudioSource* source, std::string* error) override {
    if (!connect(error)) return nullptr;
    SinkRec rec;
    if (!find_sink(device, &rec, error)) return nullptr;
    latency_s_ = latency_s;
    return make(false, channels, rec.rate, rec.name, rec.name, PA_INVALID_INDEX_, nullptr, source, error);
  }

  const PulseApi& api() const { return pa_; }
  pa_threaded_mainloop* mainloop() const { return ml_; }
  double render_latency() const { return latency_s_; }

 private:
  bool load(std::string* why) {
    if (lib_) return true;
    if (lib_tried_) {
      if (why) *why = lib_error_;
      return false;
    }
    lib_tried_ = true;
    void* h = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
      lib_error_ = std::string(kNeeds) + ": libpulse.so.0 is not installed.";
      if (why) *why = lib_error_;
      return false;
    }
    bool ok = true;
    auto sym = [&](auto& fn, const char* name) {
      void* p = dlsym(h, name);
      if (!p) ok = false;
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
    };
    sym(pa_.threaded_mainloop_new, "pa_threaded_mainloop_new");
    sym(pa_.threaded_mainloop_free, "pa_threaded_mainloop_free");
    sym(pa_.threaded_mainloop_start, "pa_threaded_mainloop_start");
    sym(pa_.threaded_mainloop_stop, "pa_threaded_mainloop_stop");
    sym(pa_.threaded_mainloop_lock, "pa_threaded_mainloop_lock");
    sym(pa_.threaded_mainloop_unlock, "pa_threaded_mainloop_unlock");
    sym(pa_.threaded_mainloop_get_api, "pa_threaded_mainloop_get_api");
    sym(pa_.proplist_new, "pa_proplist_new");
    sym(pa_.proplist_free, "pa_proplist_free");
    sym(pa_.proplist_sets, "pa_proplist_sets");
    sym(pa_.proplist_gets, "pa_proplist_gets");
    sym(pa_.context_new_with_proplist, "pa_context_new_with_proplist");
    sym(pa_.context_unref, "pa_context_unref");
    sym(pa_.context_connect, "pa_context_connect");
    sym(pa_.context_disconnect, "pa_context_disconnect");
    sym(pa_.context_get_state, "pa_context_get_state");
    sym(pa_.context_errno, "pa_context_errno");
    sym(pa_.context_set_state_callback, "pa_context_set_state_callback");
    sym(pa_.context_set_subscribe_callback, "pa_context_set_subscribe_callback");
    sym(pa_.context_subscribe, "pa_context_subscribe");
    sym(pa_.context_get_server_info, "pa_context_get_server_info");
    sym(pa_.context_get_sink_info_list, "pa_context_get_sink_info_list");
    sym(pa_.context_get_sink_info_by_index, "pa_context_get_sink_info_by_index");
    sym(pa_.context_get_sink_input_info_list, "pa_context_get_sink_input_info_list");
    sym(pa_.context_get_sink_input_info, "pa_context_get_sink_input_info");
    sym(pa_.operation_unref, "pa_operation_unref");
    sym(pa_.strerror, "pa_strerror");
    sym(pa_.get_library_version, "pa_get_library_version");
    sym(pa_.stream_new_with_proplist, "pa_stream_new_with_proplist");
    sym(pa_.stream_unref, "pa_stream_unref");
    sym(pa_.stream_connect_record, "pa_stream_connect_record");
    sym(pa_.stream_connect_playback, "pa_stream_connect_playback");
    sym(pa_.stream_disconnect, "pa_stream_disconnect");
    sym(pa_.stream_get_state, "pa_stream_get_state");
    sym(pa_.stream_set_state_callback, "pa_stream_set_state_callback");
    sym(pa_.stream_set_read_callback, "pa_stream_set_read_callback");
    sym(pa_.stream_set_write_callback, "pa_stream_set_write_callback");
    sym(pa_.stream_peek, "pa_stream_peek");
    sym(pa_.stream_drop, "pa_stream_drop");
    sym(pa_.stream_readable_size, "pa_stream_readable_size");
    sym(pa_.stream_begin_write, "pa_stream_begin_write");
    sym(pa_.stream_write, "pa_stream_write");
    sym(pa_.stream_set_monitor_stream, "pa_stream_set_monitor_stream");
    sym(pa_.stream_get_latency, "pa_stream_get_latency");
    sym(pa_.stream_get_buffer_attr, "pa_stream_get_buffer_attr");
    if (!ok) {
      lib_error_ = std::string(kNeeds) + ": the installed libpulse.so.0 is too old.";
      if (why) *why = lib_error_;
      return false;
    }
    lib_ = h;
    return true;
  }

  std::string not_running_text() {
    std::lock_guard<std::mutex> l(mu_);
    return not_running_text_locked();
  }
  std::string not_running_text_locked() const {
    if (state_ == PA_CONTEXT_READY_) return std::string(kNeeds) + ": the sound server does not answer.";
    if (state_ == PA_CONTEXT_FAILED_ || state_ == PA_CONTEXT_TERMINATED_)
      return std::string(kNeeds) + ", which is not running" + (error_.empty() ? "" : " (" + error_ + ")") + ".";
    return std::string(kNeeds) + ": still connecting to the sound server.";
  }

  // Under the mainloop lock.
  void drop_context() {
    if (!ctx_) return;
    pa_.context_set_state_callback(ctx_, nullptr, nullptr);
    pa_.context_set_subscribe_callback(ctx_, nullptr, nullptr);
    pa_.context_disconnect(ctx_);
    pa_.context_unref(ctx_);
    ctx_ = nullptr;
    std::lock_guard<std::mutex> l(mu_);
    state_ = -1;
    synced_ = false;
    sinks_.clear();
    inputs_.clear();
  }

  bool find_sink(const std::string& device, SinkRec* out, std::string* error) {
    std::lock_guard<std::mutex> l(mu_);
    const bool def = device.empty() || device == "default";
    const std::string want = def ? default_sink_ : device;
    for (const auto& kv : sinks_)
      if (kv.second.name == want) {
        *out = kv.second;
        return true;
      }
    if (error) *error = def ? "There is no output device" : "The device is not connected";
    return false;
  }

  std::unique_ptr<Stream> make(bool capture, unsigned channels, unsigned rate, const std::string& pa_device,
                               const std::string& device, uint32_t input, AudioSink* sink, AudioSource* src,
                               std::string* error) {
    std::unique_ptr<PulseStream> s(
        new PulseStream(this, capture, channels, rate, pa_device, device, input, sink, src));
    pa_.threaded_mainloop_lock(ml_);
    const bool ok = ctx_ && s->create(ctx_, error);
    pa_.threaded_mainloop_unlock(ml_);
    if (!ok) {
      if (error && error->empty()) *error = "Cannot open a stream";
      return nullptr;
    }
    return std::unique_ptr<Stream>(s.release());
  }

  // ---- mainloop thread ------------------------------------------------------------------
  static void context_state_cb(pa_context* c, void* u) {
    auto* self = static_cast<PulseBackend*>(u);
    if (c != self->ctx_) return;
    const PulseApi& pa = self->pa_;
    const int st = pa.context_get_state(c);
    if (st == PA_CONTEXT_READY_) {
      {
        std::lock_guard<std::mutex> l(self->mu_);
        self->pending_ = 3;
      }
      self->unref(pa.context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK_ | PA_SUBSCRIPTION_MASK_SINK_INPUT_ |
                                              PA_SUBSCRIPTION_MASK_SERVER_,
                                       nullptr, nullptr));
      self->unref(pa.context_get_server_info(c, &PulseBackend::server_first_cb, self));
      self->unref(pa.context_get_sink_info_list(c, &PulseBackend::sink_list_cb, self));
      self->unref(pa.context_get_sink_input_info_list(c, &PulseBackend::input_list_cb, self));
    }
    {
      std::lock_guard<std::mutex> l(self->mu_);
      self->state_ = st;
      if (st == PA_CONTEXT_FAILED_ || st == PA_CONTEXT_TERMINATED_) {
        self->error_ = pa.strerror(pa.context_errno(c));
        self->synced_ = false;
        self->sinks_.clear();
        self->inputs_.clear();
        self->changes_.fetch_add(1, std::memory_order_acq_rel);
        self->next_try_ = 0;  // try again right away next time
      }
    }
    self->cv_.notify_all();
  }

  void unref(pa_operation* o) {
    if (o) pa_.operation_unref(o);
  }

  void list_done() {
    {
      std::lock_guard<std::mutex> l(mu_);
      if (pending_ > 0 && --pending_ == 0) synced_ = true;
    }
    cv_.notify_all();
  }

  static void server_cb(pa_context* c, const pa_server_info* i, void* u) {
    auto* self = static_cast<PulseBackend*>(u);
    if (c != self->ctx_) return;
    std::lock_guard<std::mutex> l(self->mu_);
    const std::string d = i && i->default_sink_name ? i->default_sink_name : "";
    if (d != self->default_sink_) {
      self->default_sink_ = d;
      self->defaults_.fetch_add(1, std::memory_order_acq_rel);
    }
  }
  static void server_first_cb(pa_context* c, const pa_server_info* i, void* u) {
    auto* self = static_cast<PulseBackend*>(u);
    if (c != self->ctx_) return;
    server_cb(c, i, u);
    self->list_done();
  }

  void put_sink(const pa_sink_info* i) {
    SinkRec r;
    r.index = i->index;
    r.name = i->name ? i->name : "";
    r.desc = i->description ? i->description : "";
    r.monitor = i->monitor_source_name ? i->monitor_source_name : "";
    r.rate = i->sample_spec.rate ? i->sample_spec.rate : 48000;
    r.channels = i->sample_spec.channels;
    std::lock_guard<std::mutex> l(mu_);
    const bool fresh = sinks_.find(r.index) == sinks_.end();
    sinks_[r.index] = r;
    if (fresh) changes_.fetch_add(1, std::memory_order_acq_rel);
  }
  void put_input(const pa_sink_input_info* i) {
    InputRec r;
    r.index = i->index;
    r.sink = i->sink;
    r.corked = i->corked != 0;
    auto get = [&](const char* k) {
      const char* v = i->proplist ? pa_.proplist_gets(i->proplist, k) : nullptr;
      return std::string(v ? v : "");
    };
    r.binary = get("application.process.binary");
    r.app_name = get("application.name");
    r.media = i->name ? i->name : get("media.name");
    r.pid = uint32_t(std::strtoul(get("application.process.id").c_str(), nullptr, 10));
    std::lock_guard<std::mutex> l(mu_);
    auto it = inputs_.find(r.index);
    const bool fresh = it == inputs_.end() || it->second.sink != r.sink;  // new, or moved to another sink
    inputs_[r.index] = r;
    if (fresh) changes_.fetch_add(1, std::memory_order_acq_rel);
  }

  static void sink_list_cb(pa_context* c, const pa_sink_info* i, int eol, void* u) {
    auto* self = static_cast<PulseBackend*>(u);
    if (c != self->ctx_) return;
    if (eol) return self->list_done();
    if (i) self->put_sink(i);
  }
  static void input_list_cb(pa_context* c, const pa_sink_input_info* i, int eol, void* u) {
    auto* self = static_cast<PulseBackend*>(u);
    if (c != self->ctx_) return;
    if (eol) return self->list_done();
    if (i) self->put_input(i);
  }
  static void sink_one_cb(pa_context* c, const pa_sink_info* i, int eol, void* u) {
    auto* self = static_cast<PulseBackend*>(u);
    if (c == self->ctx_ && !eol && i) self->put_sink(i);
  }
  static void input_one_cb(pa_context* c, const pa_sink_input_info* i, int eol, void* u) {
    auto* self = static_cast<PulseBackend*>(u);
    if (c == self->ctx_ && !eol && i) self->put_input(i);
  }

  static void subscribe_cb(pa_context* c, int ev, uint32_t idx, void* u) {
    auto* self = static_cast<PulseBackend*>(u);
    if (c != self->ctx_) return;
    const PulseApi& pa = self->pa_;
    const int facility = ev & PA_SUBSCRIPTION_EVENT_FACILITY_MASK_, type = ev & PA_SUBSCRIPTION_EVENT_TYPE_MASK_;
    if (facility == PA_SUBSCRIPTION_EVENT_SINK_) {
      if (type == PA_SUBSCRIPTION_EVENT_REMOVE_) {
        std::lock_guard<std::mutex> l(self->mu_);
        self->sinks_.erase(idx);
        self->changes_.fetch_add(1, std::memory_order_acq_rel);
      } else {
        self->unref(pa.context_get_sink_info_by_index(c, idx, &PulseBackend::sink_one_cb, self));
      }
    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT_) {
      if (type == PA_SUBSCRIPTION_EVENT_REMOVE_) {
        std::lock_guard<std::mutex> l(self->mu_);
        self->inputs_.erase(idx);
        self->changes_.fetch_add(1, std::memory_order_acq_rel);
      } else {
        self->unref(pa.context_get_sink_input_info(c, idx, &PulseBackend::input_one_cb, self));
      }
    } else if (facility == PA_SUBSCRIPTION_EVENT_SERVER_) {
      self->unref(pa.context_get_server_info(c, &PulseBackend::server_cb, self));
    }
  }

  PulseApi pa_{};
  void* lib_ = nullptr;
  bool lib_tried_ = false;
  std::string lib_error_;
  std::mutex connect_mu_;           // one connect() at a time
  pa_threaded_mainloop* ml_ = nullptr;
  pa_context* ctx_ = nullptr;       // under the mainloop lock
  std::atomic<int64_t> next_try_{0};
  std::atomic<double> latency_s_{0.030};
  const uint32_t self_pid_;

  // The cache: written on the mainloop thread, read anywhere. Never held
  // while taking the mainloop lock.
  std::mutex mu_;
  std::condition_variable cv_;
  int state_ = -1;  // pa_context_state_t, -1 = no context
  bool synced_ = false;
  int pending_ = 0;
  std::string error_;
  std::map<uint32_t, SinkRec> sinks_;
  std::map<uint32_t, InputRec> inputs_;
  std::string default_sink_;
  std::atomic<unsigned> changes_{0}, defaults_{0};
};

// ---- PulseStream ----------------------------------------------------------------------------

bool PulseStream::create(pa_context* ctx, std::string* error) {
  const PulseApi& pa = be_->api();
  ctx_ = ctx;
  pa_sample_spec ss;
  ss.format = PA_SAMPLE_FLOAT32LE_;
  ss.rate = rate_;
  ss.channels = uint8_t(ch_);
  pa_proplist* p = pa.proplist_new();
  pa.proplist_sets(p, "media.name", capture_ ? "Desktop audio to Dante" : "Dante receive channels");
  pa.proplist_sets(p, "application.icon_name", "virgil");
  s_ = pa.stream_new_with_proplist(ctx, capture_ ? "Desktop audio to Dante" : "Dante receive channels", &ss,
                                   nullptr, p);
  pa.proplist_free(p);
  if (!s_) {
    if (error) *error = pa.strerror(pa.context_errno(ctx));
    return false;
  }
  pa.stream_set_state_callback(s_, &PulseStream::state_cb, this);
  if (capture_) pa.stream_set_read_callback(s_, &PulseStream::read_cb, this);
  else pa.stream_set_write_callback(s_, &PulseStream::write_cb, this);
  if (monitor_input_ != PA_INVALID_INDEX_ && pa.stream_set_monitor_stream(s_, monitor_input_) < 0) {
    if (error) *error = pa.strerror(pa.context_errno(ctx));
    return false;
  }
  return true;
}

bool PulseStream::start(std::string* error) {
  const PulseApi& pa = be_->api();
  pa_threaded_mainloop* ml = be_->mainloop();
  const uint32_t fb = 4 * ch_;
  pa_buffer_attr a;
  a.maxlength = a.tlength = a.prebuf = a.minreq = a.fragsize = 0xFFFFFFFFu;
  int rc;
  pa.threaded_mainloop_lock(ml);
  if (capture_) {
    a.fragsize = fb * (rate_ / 100);  // 10 ms
    rc = pa.stream_connect_record(s_, pa_device_.c_str(), &a,
                                  PA_STREAM_ADJUST_LATENCY_ | PA_STREAM_DONT_MOVE_ | PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND_);
  } else {
    a.tlength = fb * uint32_t(double(rate_) * be_->render_latency());
    a.minreq = fb * (rate_ / 200);  // 5 ms
    rc = pa.stream_connect_playback(s_, pa_device_.c_str(), &a,
                                    PA_STREAM_ADJUST_LATENCY_ | PA_STREAM_DONT_MOVE_ | PA_STREAM_INTERPOLATE_TIMING_ |
                                        PA_STREAM_AUTO_TIMING_UPDATE_,
                                    nullptr, nullptr);
  }
  if (rc < 0 && error) *error = pa.strerror(pa.context_errno(ctx_));
  pa.threaded_mainloop_unlock(ml);
  return rc >= 0;
}

PulseStream::~PulseStream() {
  if (!s_) return;
  const PulseApi& pa = be_->api();
  pa.threaded_mainloop_lock(be_->mainloop());
  pa.stream_set_state_callback(s_, nullptr, nullptr);
  pa.stream_set_read_callback(s_, nullptr, nullptr);
  pa.stream_set_write_callback(s_, nullptr, nullptr);
  if (pa.stream_get_state(s_) != PA_STREAM_UNCONNECTED_) pa.stream_disconnect(s_);
  pa.stream_unref(s_);
  pa.threaded_mainloop_unlock(be_->mainloop());
}

void PulseStream::state_cb(pa_stream* s, void* u) {
  auto* self = static_cast<PulseStream*>(u);
  const PulseApi& pa = self->be_->api();
  const int st = pa.stream_get_state(s);
  if (st == PA_STREAM_FAILED_) {
    const int e = pa.context_errno(self->ctx_);
    // "No such entity": the app closed this stream or the device went away.
    self->fail(e == 5 ? std::string(self->monitor_input_ != PA_INVALID_INDEX_ ? "The app stopped playing"
                                                                                : "The device went away")
                      : std::string(pa.strerror(e)));
  } else if (st == PA_STREAM_TERMINATED_) {
    self->fail(self->monitor_input_ != PA_INVALID_INDEX_ ? "The app stopped playing" : "The stream was closed");
  }
}

void PulseStream::feed(const uint8_t* p, size_t n, int64_t now) {
  const size_t fb = 4 * ch_;
  if (carry_n_) {
    const size_t k = std::min(fb - carry_n_, n);
    std::memcpy(carry_ + carry_n_, p, k);
    carry_n_ += k;
    p += k;
    n -= k;
    if (carry_n_ < fb) return;
    float f[8];
    std::memcpy(f, carry_, fb);
    carry_n_ = 0;
    sink_->audio(f, 1, now);
  }
  const size_t frames = n / fb;
  if (frames) {
    if (reinterpret_cast<uintptr_t>(p) % alignof(float) == 0) {
      sink_->audio(reinterpret_cast<const float*>(p), frames, now);
    } else {
      buf_.resize(frames * ch_);
      std::memcpy(buf_.data(), p, frames * fb);
      sink_->audio(buf_.data(), frames, now);
    }
  }
  const size_t rest = n - frames * fb;
  if (rest) {
    std::memcpy(carry_, p + frames * fb, rest);
    carry_n_ = rest;
  }
}

void PulseStream::read_cb(pa_stream* s, size_t, void* u) {
  static thread_local bool raised = false;
  if (!raised) {
    raised = true;
    raise_audio_thread_priority(10000000, 30);
  }
  auto* self = static_cast<PulseStream*>(u);
  const PulseApi& pa = self->be_->api();
  for (int guard = 0; guard < 64; ++guard) {
    const void* data = nullptr;
    size_t n = 0;
    if (pa.stream_peek(s, &data, &n) < 0 || n == 0) break;
    const int64_t now = virgil::mono_ns();
    if (data) self->feed(static_cast<const uint8_t*>(data), n, now);
    else self->sink_->audio(nullptr, n / (4 * self->ch_), now);  // a gap in the recording
    pa.stream_drop(s);
  }
}

void PulseStream::write_cb(pa_stream* s, size_t nbytes, void* u) {
  static thread_local bool raised = false;
  if (!raised) {
    raised = true;
    raise_audio_thread_priority(10000000, 30);
  }
  auto* self = static_cast<PulseStream*>(u);
  const PulseApi& pa = self->be_->api();
  const size_t fb = 4 * self->ch_;
  const size_t frames = nbytes / fb;
  if (!frames) return;
  if (self->buf_.size() < frames * self->ch_) self->buf_.resize(frames * self->ch_);
  self->src_->render(self->buf_.data(), frames, virgil::mono_ns());
  pa.stream_write(s, self->buf_.data(), frames * fb, nullptr, 0, PA_SEEK_RELATIVE_);
}

}  // namespace

std::unique_ptr<Backend> make_system_backend() { return std::unique_ptr<Backend>(new PulseBackend()); }

}  // namespace desk
