#include "diagnostics.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <ctime>

#include "json.h"

namespace virgil {

namespace {

constexpr size_t kMaxEvents = 300;
constexpr size_t kMaxMinutes = 120;
constexpr int64_t kMergeMs = 3000;  // repeats within this gap extend one event
constexpr int64_t kGraceMs = 3000;  // after an engine start (it settles first)

uint64_t delta(uint64_t now, uint64_t before) { return now >= before ? now - before : now; }

std::string fmt(const char* f, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
std::string fmt(const char* f, ...) {
  char b[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

std::string clock_time(int64_t ms) {
  const std::time_t t = std::time_t(ms / 1000);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tmv);
  return b;
}

const char* plural(uint64_t n, const char* one, const char* many) { return n == 1 ? one : many; }

}  // namespace

const char* Diagnostics::kind_name(Kind k) {
  switch (k) {
    case Kind::NetLate: return "network-late";
    case Kind::NetLost: return "network-lost";
    case Kind::AppLate: return "app-late";
    case Kind::AppMissed: return "app-missed";
    case Kind::EngineLate: return "engine-late";
    case Kind::ClockJump: return "clock-jump";
    case Kind::ClockLost: return "clock-lost";
    case Kind::ClockInfo: return "clock";
    case Kind::Service: return "service";
  }
  return "?";
}

std::string Diagnostics::event_text(const Event& e, double, double rx_latency_ms) {
  switch (e.kind) {
    case Kind::NetLate:
      return fmt("%" PRIu64 " %s from the network arrived too late to play (up to %.1f ms old; receive "
                 "latency is %.1f ms)",
                 e.count, plural(e.count, "packet", "packets"), e.value, rx_latency_ms);
    case Kind::NetLost:
      return fmt("%.1f ms of audio from the network never arrived (%" PRIu64 " %s)", e.value, e.count,
                 plural(e.count, "gap", "gaps"));
    case Kind::AppLate:
      return fmt("%s delivered %.1f ms of audio too late for the network; it was dropped", e.text.c_str(),
                 e.value);
    case Kind::AppMissed:
      return fmt("%s did not deliver its audio in time (%.1f ms of silence sent)", e.text.c_str(), e.value);
    case Kind::EngineLate:
      return fmt("Virgil's audio engine was woken up too late %" PRIu64 " %s (the computer was busy); "
                 "audio from apps missed the network",
                 e.count, plural(e.count, "time", "times"));
    case Kind::ClockJump:
      return fmt("The media clock had to be realigned%s (audio paused briefly)",
                 e.count > 1 ? fmt(" %" PRIu64 " times", e.count).c_str() : "");
    default:
      return e.text;
  }
}

void Diagnostics::bump_minute_locked(Kind k, int64_t now) {
  const int64_t m0 = now - now % 60000;
  if (minutes_.empty() || minutes_.back().start_ms != m0) {
    Minute m;
    m.start_ms = m0;
    minutes_.push_back(m);
    while (minutes_.size() > kMaxMinutes) minutes_.pop_front();
  }
  Minute& m = minutes_.back();
  switch (k) {
    case Kind::NetLate:
    case Kind::NetLost: ++m.net; break;
    case Kind::AppLate:
    case Kind::AppMissed: ++m.apps; break;
    case Kind::EngineLate: ++m.engine; break;
    case Kind::ClockJump:
    case Kind::ClockLost: ++m.clock; break;
    default: break;
  }
}

void Diagnostics::add_locked(Kind k, const std::string& key, uint64_t count, double value, int64_t now,
                             const std::string& text) {
  if (is_dropout(k)) bump_minute_locked(k, now);
  // Repeats of the same trouble in consecutive seconds are one event.
  for (auto it = events_.rbegin(); it != events_.rend() && now - it->end_ms <= kMergeMs; ++it) {
    if (it->kind != k || it->key != key || !is_dropout(k)) continue;
    it->end_ms = now;
    it->count += count;
    it->value = k == Kind::NetLate ? std::max(it->value, value) : it->value + value;
    ++it->seconds;
    return;
  }
  Event e;
  e.start_ms = e.end_ms = now;
  e.kind = k;
  e.key = key;
  e.text = text;
  e.count = count;
  e.value = value;
  e.seconds = 1;
  events_.push_back(e);
  while (events_.size() > kMaxEvents) events_.pop_front();
  if (is_dropout(k)) ++totals_[k];
}

void Diagnostics::note(Kind k, const std::string& text, int64_t now) {
  std::lock_guard<std::mutex> l(m_);
  if (!since_ms_) since_ms_ = now;
  add_locked(k, std::string(), 0, 0, now, text);
}

void Diagnostics::reset(int64_t now) {
  std::lock_guard<std::mutex> l(m_);
  events_.clear();
  minutes_.clear();
  totals_.clear();
  worst_age_ms_ = 0;
  since_ms_ = now;
}

void Diagnostics::engine_started(const Config& cfg, int64_t now) {
  std::lock_guard<std::mutex> l(m_);
  if (!since_ms_) since_ms_ = now;
  have_prev_ = false;
  prev_apps_.clear();
  prev_state_ = 0;
  prev_gm_.clear();
  grace_until_ = now + kGraceMs;
  period_ms_ = double(cfg.period_frames()) * 1000.0 / cfg.sample_rate;
  rx_latency_ms_ = cfg.latency_us / 1000.0;
  tx_latency_ms_ = cfg.tx_latency_us / 1000.0;
  add_locked(Kind::Service, std::string(), 0, 0, now,
             fmt("Audio engine started: %.1f kHz, %u out / %u in, receive latency %.1f ms, transmit latency "
                 "%.1f ms",
                 cfg.sample_rate / 1000.0, cfg.tx_channels, cfg.rx_channels, rx_latency_ms_, tx_latency_ms_));
}

void Diagnostics::sample(const EngineCounters& c, const Config& cfg, int64_t now) {
  std::lock_guard<std::mutex> l(m_);
  if (!since_ms_) since_ms_ = now;
  const double rate = cfg.sample_rate ? cfg.sample_rate : 48000;
  const bool grace = now < grace_until_;

  // Clock state changes.
  if (c.state != prev_state_ || (c.state == kStatePtpLocked && c.grandmaster != prev_gm_)) {
    if (prev_state_ != 0) {
      if (c.state == kStatePtpLocked && prev_state_ == kStatePtpLocked)
        add_locked(Kind::ClockInfo, std::string(), 0, 0, now,
                   "Clock leader changed from " + prev_gm_ + " to " + c.grandmaster);
      else if (c.state == kStatePtpLocked)
        add_locked(Kind::ClockInfo, std::string(), 0, 0, now, "Locked to the Dante clock leader " + c.grandmaster);
      else if (c.state == kStatePtpMaster)
        add_locked(Kind::ClockInfo, std::string(), 0, 0, now, "Virgil is now the clock leader");
      else if (c.state == kStateFreeRun && prev_state_ == kStatePtpLocked && cfg.clock == "ptp")
        add_locked(Kind::ClockLost, std::string(), 1, 0, now,
                   "Lost the Dante clock leader " + prev_gm_ +
                       "; Virgil runs on its own clock (not in sync with the network) until it is back");
    } else if (c.state == kStatePtpLocked) {
      add_locked(Kind::ClockInfo, std::string(), 0, 0, now, "Locked to the Dante clock leader " + c.grandmaster);
    }
    prev_state_ = c.state;
    prev_gm_ = c.grandmaster;
  }

  if (!have_prev_) {
    prev_ = c;
    have_prev_ = true;
    for (const auto& a : c.apps) prev_apps_[a.pid] = {a.underrun_ticks, a.late_frames};
    return;
  }

  if (!grace) {
    const uint64_t late = delta(c.rx_late_packets, prev_.rx_late_packets);
    if (late) {
      const double age = c.rx_max_age_samples * 1000.0 / rate;
      worst_age_ms_ = std::max(worst_age_ms_, age);
      add_locked(Kind::NetLate, std::string(), late, age, now, std::string());
    }
    const uint64_t gaps = delta(c.rx_gaps, prev_.rx_gaps);
    if (gaps)
      add_locked(Kind::NetLost, std::string(), gaps, delta(c.rx_missing_samples, prev_.rx_missing_samples) * 1000.0 / rate,
                 now, std::string());
    // Only audible with an app attached (otherwise nothing waited for it).
    const uint64_t lt = delta(c.overdue_ticks, prev_.overdue_ticks);
    if (lt && !c.apps.empty()) add_locked(Kind::EngineLate, std::string(), lt, 0, now, std::string());
    const uint64_t rs = delta(c.resyncs, prev_.resyncs);
    if (rs) add_locked(Kind::ClockJump, std::string(), rs, 0, now, std::string());
  }

  std::map<uint32_t, AppPrev> apps;
  for (const auto& a : c.apps) {
    apps[a.pid] = {a.underrun_ticks, a.late_frames};
    auto it = prev_apps_.find(a.pid);
    if (it == prev_apps_.end() || grace) continue;  // new app: its baseline
    const std::string who = a.name.empty() ? fmt("An app (process %u)", a.pid) : a.name;
    const uint64_t miss = delta(a.underrun_ticks, it->second.underrun_ticks);
    if (miss) add_locked(Kind::AppMissed, who, miss, miss * period_ms_, now, who);
    const uint64_t lf = delta(a.late_frames, it->second.late_frames);
    if (lf) add_locked(Kind::AppLate, who, lf, lf * 1000.0 / rate, now, who);
  }
  prev_apps_.swap(apps);
  prev_ = c;
}

uint64_t Diagnostics::recent_dropouts(int64_t now, int64_t seconds) const {
  std::lock_guard<std::mutex> l(m_);
  uint64_t n = 0;
  for (const auto& e : events_)
    if (is_dropout(e.kind) && now - e.end_ms <= seconds * 1000) ++n;
  return n;
}

std::vector<std::string> Diagnostics::hints_locked(int64_t now) const {
  std::vector<std::string> h;
  double age = 0;
  bool lost = false, engine = false, clock = false;
  std::vector<std::string> apps;
  for (const auto& e : events_) {
    if (now - e.end_ms > 3600 * 1000) continue;
    switch (e.kind) {
      case Kind::NetLate: age = std::max(age, e.value); break;
      case Kind::NetLost: lost = true; break;
      case Kind::EngineLate: engine = true; break;
      case Kind::ClockJump:
      case Kind::ClockLost: clock = true; break;
      case Kind::AppLate:
      case Kind::AppMissed:
        if (std::find(apps.begin(), apps.end(), e.key) == apps.end()) apps.push_back(e.key);
        break;
      default: break;
    }
  }
  if (age > 0) {
    static const double steps[] = {1, 2, 4, 5, 10, 20};
    double want = 0;
    for (double s : steps)
      if (s > rx_latency_ms_ && s >= age * 1.2 + 0.5) {
        want = s;
        break;
      }
    if (want > 0)
      h.push_back(fmt("Audio from the network arrived up to %.1f ms late for the receive latency of %.1f ms. "
                      "Raise Receive latency to %g ms in Settings.",
                      age, rx_latency_ms_, want));
    else
      h.push_back(fmt("Audio from the network arrived up to %.1f ms late. That is more than any latency setting "
                      "covers: look for a slow or overloaded network path (Wi-Fi, a busy or misconfigured "
                      "switch).",
                      age));
  }
  if (lost)
    h.push_back("Audio packets are being lost on the way. Use wired Ethernet (not Wi-Fi), check cables, and turn "
                "off Energy-Efficient Ethernet (EEE, \"green Ethernet\") on the switch ports.");
  for (const auto& a : apps)
    h.push_back(a + fmt(" did not keep up. Raise its buffer size, or raise Virgil's transmit latency (now %.1f ms).",
                        tx_latency_ms_));
  if (engine)
#if defined(_WIN32)
    h.push_back("The computer kept Virgil's audio engine waiting. Close heavy programs, choose the High "
                "performance power plan and keep a laptop plugged in.");
#elif defined(__APPLE__)
    h.push_back("The computer kept Virgil's audio engine waiting. Close heavy programs and keep a laptop "
                "plugged in (Low Power Mode slows audio threads).");
#else
    h.push_back("The computer kept Virgil's audio engine waiting. Close heavy programs, set the CPU governor to "
                "performance, and check that virgild has real-time priority (see the README).");
#endif
  if (clock)
    h.push_back("The clock was disturbed. Keep the Dante clock leader connected, and have only one network "
                "adapter of this computer on the Dante network.");
  return h;
}

std::string Diagnostics::json(int64_t now) const {
  std::lock_guard<std::mutex> l(m_);
  Json j;
  j.begin_obj();
  j.kv("since_ms", (unsigned long long)since_ms_);
  j.kv("now_ms", (unsigned long long)now);
  uint64_t hour = 0;
  for (const auto& e : events_)
    if (is_dropout(e.kind) && now - e.end_ms <= 3600 * 1000) ++hour;
  j.kv("dropouts_last_hour", (unsigned long long)hour);
  uint64_t total = 0;
  j.key("totals").begin_obj();
  for (const auto& t : totals_) {
    j.kv(kind_name(t.first), (unsigned long long)t.second);
    total += t.second;
  }
  j.end_obj();
  j.kv("dropouts_total", (unsigned long long)total);
  j.kv("rx_latency_ms", rx_latency_ms_);
  j.kv("tx_latency_ms", tx_latency_ms_);
  j.kv("worst_network_age_ms", worst_age_ms_);
  j.key("hints").begin_arr();
  for (const auto& s : hints_locked(now)) j.v(s);
  j.end_arr();
  j.key("events").begin_arr();
  for (auto it = events_.rbegin(); it != events_.rend(); ++it) {  // newest first
    j.begin_obj();
    j.kv("start_ms", (unsigned long long)it->start_ms).kv("end_ms", (unsigned long long)it->end_ms);
    j.kv("kind", kind_name(it->kind)).kvb("dropout", is_dropout(it->kind));
    j.kv("severity", it->kind == Kind::ClockLost ? "error" : is_dropout(it->kind) ? "warn" : "info");
    j.kv("text", event_text(*it, period_ms_, rx_latency_ms_));
    j.end_obj();
  }
  j.end_arr();
  j.key("minutes").begin_arr();
  for (const auto& m : minutes_) {
    if (now - m.start_ms > 3600 * 1000) continue;
    j.begin_obj().kv("t", (unsigned long long)m.start_ms);
    j.kv("network", (unsigned long long)m.net).kv("apps", (unsigned long long)m.apps);
    j.kv("engine", (unsigned long long)m.engine).kv("clock", (unsigned long long)m.clock).end_obj();
  }
  j.end_arr();
  j.end_obj();
  return j.take();
}

std::string Diagnostics::report(int64_t now) const {
  std::lock_guard<std::mutex> l(m_);
  std::string r;
  r += "Diagnostics since " + clock_time(since_ms_) + "\n";
  uint64_t total = 0;
  for (const auto& t : totals_) {
    r += fmt("  %-12s %" PRIu64 "\n", kind_name(t.first), t.second);
    total += t.second;
  }
  if (!total) r += "  no dropouts\n";
  r += fmt("  worst network packet age %.1f ms (receive latency %.1f ms)\n", worst_age_ms_, rx_latency_ms_);
  const auto hints = hints_locked(now);
  if (!hints.empty()) {
    r += "\nSuggestions\n";
    for (const auto& s : hints) r += "  - " + s + "\n";
  }
  r += "\nEvents (oldest first)\n";
  for (const auto& e : events_) {
    r += "  " + clock_time(e.start_ms);
    if (e.end_ms - e.start_ms >= 1000) r += fmt(" (+%llds)", (long long)((e.end_ms - e.start_ms) / 1000));
    r += fmt("  [%s] ", kind_name(e.kind)) + event_text(e, period_ms_, rx_latency_ms_) + "\n";
  }
  if (events_.empty()) r += "  none\n";
  return r;
}

}  // namespace virgil
