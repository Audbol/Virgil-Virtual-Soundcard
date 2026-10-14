#include "diagnostics.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
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

// Up to the next 0.5 ms (a hair of slack for floating-point noise).
double ceil_half_ms(double ms) { return std::ceil(ms * 2 - 1e-6) / 2; }

std::string duration_text(double s) {
  if (s < 3600) return fmt("%d min", int(s / 60));
  return fmt("%.1f h", s / 3600);
}

}  // namespace

const char* tx_suggestion_state_name(TxLatencySuggestion::State s) {
  return s == TxLatencySuggestion::State::Ready ? "ready" : "collecting";
}

const char* tx_suggestion_direction_name(TxLatencySuggestion::Direction d) {
  switch (d) {
    case TxLatencySuggestion::Direction::Lower: return "lower";
    case TxLatencySuggestion::Direction::Raise: return "raise";
    default: return "keep";
  }
}

TxLatencySuggestion suggest_tx_latency(const TxLatencyObservation& o) {
  using S = TxLatencySuggestion;
  S r;
  Config c;
  c.tick_us = o.tick_us;
  const double send_delay_ms = c.tx_send_delay_us() / 1000.0;
  r.observed_s = o.observed_s;
  r.needed_s = kTxSuggestNeededS;
  r.current_ms = o.current_ms;
  r.min_ms = c.min_tx_latency_us() / 1000.0;
  r.late_sends = o.late_sends;
  r.worst_lag_ms = o.worst_lag_ms;
  if (o.observed_s < kTxSuggestNeededS) {
    r.text = fmt("Transmit latency suggestion: measuring, %d of %d min (counts while a Dante device receives "
                 "Virgil's channels)",
                 int(o.observed_s / 60), int(kTxSuggestNeededS / 60));
    return r;
  }
  r.state = S::State::Ready;
  const double hours = o.observed_s / 3600;
  r.confidence = hours < 2 ? "low" : hours < 8 ? "medium" : "high";
  const std::string over = fmt("over %s of sending, %s confidence", duration_text(o.observed_s).c_str(), r.confidence);

  double need = send_delay_ms + o.worst_lag_ms + kTxSuggestMarginMs;
  if (o.late_sends) {
    // Audio was dropped: never keep (or lower) the setting that dropped it.
    need = std::max(need, send_delay_ms + o.worst_late_ms + kTxSuggestMarginMs);
    need = std::max(need, o.current_ms + 0.5);
  }
  double want = std::max(ceil_half_ms(need), ceil_half_ms(r.min_ms));
  want = std::min(want, kTxSuggestMaxMs);

  if (want > o.current_ms + 1e-6) {
    r.direction = S::Direction::Raise;
    r.suggested_ms = want;
    const std::string why =
        o.late_sends ? fmt("%" PRIu64 " late %s dropped audio", o.late_sends, plural(o.late_sends, "send", "sends"))
                     : fmt("the sending thread woke up to %.1f ms late, leaving less than %.0f ms of margin",
                           o.worst_lag_ms, kTxSuggestMarginMs);
    r.text = fmt("Transmit latency should be raised to %g ms (now %g ms): %s (%s).", want, o.current_ms, why.c_str(),
                 over.c_str());
  } else if (o.current_ms - want >= 0.5 - 1e-6) {
    // Only steps of at least 0.5 ms down, so the suggestion does not churn.
    r.direction = S::Direction::Lower;
    r.suggested_ms = want;
    r.text = fmt("Transmit latency can be lowered to %g ms (now %g ms): the sending thread woke up at most %.1f ms "
                 "late (%s).",
                 want, o.current_ms, o.worst_lag_ms, over.c_str());
  } else {
    r.suggested_ms = o.current_ms;
    if (o.late_sends)  // at the 40 ms maximum already
      r.text = fmt("The sending thread woke up later than any transmit latency covers (%" PRIu64
                   " late %s): the computer is too busy.",
                   o.late_sends, plural(o.late_sends, "send", "sends"));
    else
      r.text = fmt("The transmit latency of %g ms is right for this computer: the sending thread woke up at most "
                   "%.1f ms late (%s).",
                   o.current_ms, o.worst_lag_ms, over.c_str());
  }
  return r;
}

const char* Diagnostics::kind_name(Kind k) {
  switch (k) {
    case Kind::NetLate: return "network-late";
    case Kind::NetLost: return "network-lost";
    case Kind::AppLate: return "app-late";
    case Kind::AppMissed: return "app-missed";
    case Kind::EngineLate: return "engine-late";
    case Kind::NetSendLate: return "network-send-late";
    case Kind::ClockJump: return "clock-jump";
    case Kind::ClockLost: return "clock-lost";
    case Kind::ClockInfo: return "clock";
    case Kind::Service: return "service";
  }
  return "?";
}

std::string Diagnostics::event_text(const Event& e, double, double rx_latency_ms, double tx_latency_ms) {
  switch (e.kind) {
    case Kind::NetLate:
      if (e.extra > 0)
        return fmt("%.1f ms of audio from the network arrived too late and was lost (packets up to %.1f ms old; "
                   "receive latency is %.1f ms)",
                   e.value, e.extra, rx_latency_ms);
      return fmt("%.1f ms of audio from the network arrived too late and was lost (receive latency is %.1f ms)",
                 e.value, rx_latency_ms);
    case Kind::NetLost:
      return fmt("%.1f ms of audio from the network never arrived (%" PRIu64 " %s)", e.value, e.count,
                 plural(e.count, "gap", "gaps"));
    case Kind::AppLate:
      return fmt("%s delivered %.1f ms of audio too late for the network; it was dropped", e.text.c_str(),
                 e.value);
    case Kind::AppMissed:
      return fmt("%s did not deliver its audio in time (%.1f ms of silence sent)", e.text.c_str(), e.value);
    case Kind::EngineLate: {
      const std::string ms = e.value >= 10 ? fmt("%.0f ms", e.value) : fmt("%.1f ms", e.value);
      if (e.count <= 1)
        return "The computer stalled ~" + ms +
               ": Virgil's audio engine and apps waited, so audio to the network had a gap";
      return fmt("The computer stalled %" PRIu64 " times, up to ~%s: Virgil's audio engine and apps waited, so "
                 "audio to the network had gaps",
                 e.count, ms.c_str());
    }
    case Kind::NetSendLate:
      return fmt("Audio to the network was dropped: Virgil's sending thread woke up to %.1f ms late %" PRIu64
                 " %s (transmit latency is %.1f ms)",
                 e.value, e.count, plural(e.count, "time", "times"), tx_latency_ms);
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
    case Kind::EngineLate:
    case Kind::NetSendLate: ++m.engine; break;  // a busy computer
    case Kind::ClockJump:
    case Kind::ClockLost: ++m.clock; break;
    default: break;
  }
}

void Diagnostics::add_locked(Kind k, const std::string& key, uint64_t count, double value, int64_t now,
                             const std::string& text, double extra) {
  if (is_dropout(k)) bump_minute_locked(k, now);
  // Repeats of the same trouble in consecutive seconds are one event.
  for (auto it = events_.rbegin(); it != events_.rend() && now - it->end_ms <= kMergeMs; ++it) {
    if (it->kind != k || it->key != key || !is_dropout(k)) continue;
    it->end_ms = now;
    it->count += count;
    // Largest lag for late wake-ups; amounts (ms lost, silence) add up.
    it->value = k == Kind::NetSendLate || k == Kind::EngineLate ? std::max(it->value, value) : it->value + value;
    it->extra = std::max(it->extra, extra);
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
  e.extra = extra;
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
  worst_send_lag_ms_ = 0;
  near_late_packets_ = 0;
  worst_stall_ms_ = 0;
  worst_wakeup_lag_ms_ = 0;
  for (auto& n : send_lag_over_) n = 0;
  reset_tx_observation_locked(tx_tick_us_);
  since_ms_ = now;
}

void Diagnostics::reset_tx_observation_locked(uint32_t tick_us) {
  const double current = tx_obs_.current_ms;
  tx_obs_ = TxLatencyObservation();
  tx_obs_.tick_us = tick_us;
  tx_obs_.current_ms = current;
}

TxLatencySuggestion Diagnostics::tx_suggestion_locked() const { return suggest_tx_latency(tx_obs_); }

TxLatencySuggestion Diagnostics::tx_latency_suggestion() const {
  std::lock_guard<std::mutex> l(m_);
  return tx_suggestion_locked();
}

void Diagnostics::engine_started(const Config& cfg, int64_t now) {
  std::lock_guard<std::mutex> l(m_);
  if (!since_ms_) since_ms_ = now;
  have_prev_ = false;
  prev_apps_.clear();
  prev_state_ = 0;
  prev_gm_.clear();
  last_stall_ms_ = 0;
  grace_until_ = now + kGraceMs;
  period_ms_ = double(cfg.period_frames()) * 1000.0 / cfg.sample_rate;
  rx_latency_ms_ = cfg.latency_us / 1000.0;
  tx_latency_ms_ = cfg.tx_latency_us / 1000.0;
  // The send delay depends on the tick: what was observed at another tick
  // does not carry over. A new transmit latency keeps it.
  tx_tick_us_ = cfg.tick_us;
  if (tx_obs_.tick_us != cfg.tick_us) reset_tx_observation_locked(cfg.tick_us);
  tx_obs_.current_ms = tx_latency_ms_;
  add_locked(Kind::Service, std::string(), 0, 0, now,
             fmt("Audio engine started: %.1f kHz, %u out / %u in, receive latency %.1f ms, transmit latency "
                 "%.1f ms%s",
                 cfg.sample_rate / 1000.0, cfg.tx_channels, cfg.rx_channels, rx_latency_ms_, tx_latency_ms_,
                 cfg.tick_us != 1000 ? fmt(", mixing tick %g ms", cfg.tick_us / 1000.0).c_str() : ""));
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
      // Lost only when the leader is gone; free-run with a leader is a
      // re-lock after a clock step (reported as a clock jump).
      else if (c.state == kStateFreeRun && prev_state_ == kStatePtpLocked && cfg.clock == "ptp" &&
               c.grandmaster.empty())
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
    // Packets older than the receive latency: usually still in time for the
    // engine's second copy, so only a statistic (close to the limit). What
    // arrived after its last copy was lost: that is the dropout.
    const double age = c.rx_max_age_samples * 1000.0 / rate;
    const uint64_t late = delta(c.rx_late_packets, prev_.rx_late_packets);
    if (late) {
      near_late_packets_ += late;
      worst_age_ms_ = std::max(worst_age_ms_, age);
    }
    const uint64_t lost_late = delta(c.rx_lost_late_samples, prev_.rx_lost_late_samples);
    if (lost_late)
      add_locked(Kind::NetLate, std::string(), lost_late, lost_late * 1000.0 / rate, now, std::string(),
                 late ? age : 0);
    const uint64_t gaps = delta(c.rx_gaps, prev_.rx_gaps);
    if (gaps)
      add_locked(Kind::NetLost, std::string(), gaps, delta(c.rx_missing_samples, prev_.rx_missing_samples) * 1000.0 / rate,
                 now, std::string());
    // Only audible with an app attached (otherwise nothing waited for it).
    // One stall and the catch-up ticks after it are one episode.
    uint64_t stalls = delta(c.overdue_episodes, prev_.overdue_episodes);
    if (!stalls && delta(c.overdue_ticks, prev_.overdue_ticks)) stalls = 1;
    if (stalls && !c.apps.empty()) {
      const double ms = c.max_late_ns / 1e6;
      worst_stall_ms_ = std::max(worst_stall_ms_, ms);
      last_stall_ms_ = now;
      add_locked(Kind::EngineLate, std::string(), stalls, ms, now, std::string());
    }
    // The Dante sending thread woke up after the transmit latency: Inferno
    // skipped that audio. Like EngineLate, only audible with an app attached.
    const uint64_t sl = delta(c.tx_late_wakeups, prev_.tx_late_wakeups);
    if (sl && !c.apps.empty()) {
      const double lag = c.tx_max_lag_samples * 1000.0 / rate;
      worst_send_lag_ms_ = std::max(worst_send_lag_ms_, lag);
      add_locked(Kind::NetSendLate, std::string(), sl, lag, now, std::string());
    }
    // Sending thread near misses: a statistic for the report, not dropouts.
    const double wake_lag = c.tx_max_wakeup_lag_samples * 1000.0 / rate;
    worst_wakeup_lag_ms_ = std::max(worst_wakeup_lag_ms_, wake_lag);
    for (int i = 0; i < 4; ++i) send_lag_over_[i] += delta(c.tx_lag_over[i], prev_.tx_lag_over[i]);
    const uint64_t rs = delta(c.resyncs, prev_.resyncs);
    if (rs) add_locked(Kind::ClockJump, std::string(), rs, 0, now, std::string());
    // Transmit latency suggestion: seconds in which packets went out. A
    // second with a clock step or clock state change is left out (the "lag"
    // of a clock step is not the thread waking late).
    const bool clock_moved = rs || delta(c.tx_clock_jumps, prev_.tx_clock_jumps) || c.state != prev_.state;
    if (cfg.tx_channels && delta(c.tx_running_wakeups, prev_.tx_running_wakeups) && !clock_moved) {
      tx_obs_.observed_s += 1;
      tx_obs_.worst_lag_ms = std::max(tx_obs_.worst_lag_ms, wake_lag);
      if (sl) {
        tx_obs_.late_sends += sl;
        tx_obs_.worst_late_ms = std::max(tx_obs_.worst_late_ms, c.tx_max_lag_samples * 1000.0 / rate);
      }
    }
  }

  std::map<uint32_t, AppPrev> apps;
  for (const auto& a : c.apps) {
    apps[a.pid] = {a.underrun_ticks, a.late_frames};
    auto it = prev_apps_.find(a.pid);
    if (it == prev_apps_.end() || grace) continue;  // new app: its baseline
    // The engine stalled this second (or just before the sample): apps
    // waited with it, it is not their fault (reported as the stall).
    if (last_stall_ms_ && now - last_stall_ms_ <= 1500) continue;
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
  double age = 0, send_lag = 0;
  bool net_late = false, lost = false, engine = false, clock = false;
  std::vector<std::string> apps;
  for (const auto& e : events_) {
    if (now - e.end_ms > 3600 * 1000) continue;
    switch (e.kind) {
      case Kind::NetLate:
        net_late = true;
        age = std::max(age, e.extra);
        break;
      case Kind::NetLost: lost = true; break;
      case Kind::EngineLate: engine = true; break;
      case Kind::NetSendLate: send_lag = std::max(send_lag, e.value); break;
      case Kind::ClockJump:
      case Kind::ClockLost: clock = true; break;
      case Kind::AppLate:
      case Kind::AppMissed:
        if (std::find(apps.begin(), apps.end(), e.key) == apps.end()) apps.push_back(e.key);
        break;
      default: break;
    }
  }
  // Only when audio was actually lost: packets merely close to the limit
  // still played.
  if (net_late && age <= 0) age = worst_age_ms_;
  if (net_late && age <= 0)
    h.push_back(fmt("Audio from the network arrived too late to play for the receive latency of %.1f ms. Raise "
                    "Receive latency in Settings.",
                    rx_latency_ms_));
  if (net_late && age > 0) {
    static const double steps[] = {1, 2, 4, 5, 10, 20};
    double want = 0;
    for (double s : steps)
      if (s > rx_latency_ms_ && s >= age * 1.2 + 0.5) {
        want = s;
        break;
      }
    if (want > 0)
      h.push_back(fmt("Audio from the network arrived too late and was lost (packets up to %.1f ms old; receive "
                      "latency %.1f ms). Raise Receive latency to %g ms in Settings.",
                      age, rx_latency_ms_, want));
    else
      h.push_back(fmt("Audio from the network arrived too late and was lost (packets up to %.1f ms old). That is more than any latency setting "
                      "covers: look for a slow or overloaded network path (Wi-Fi, a busy or misconfigured "
                      "switch).",
                      age));
  }
  // The transmit latency suggestion, once it has seen enough sending; with
  // late sends the hint below names its value instead of a rough one.
  const TxLatencySuggestion sug = tx_suggestion_locked();
  const bool sug_ready = sug.state == TxLatencySuggestion::State::Ready;
  if (send_lag > 0) {
    static const double steps[] = {1, 2, 4, 5, 10, 20, 40};
    double want = 0;
    if (sug_ready && sug.direction == TxLatencySuggestion::Direction::Raise && sug.suggested_ms > tx_latency_ms_) {
      want = sug.suggested_ms;
    } else {
      for (double s : steps)
        if (s > tx_latency_ms_ && s >= send_lag * 1.2 + 0.5) {
          want = s;
          break;
        }
    }
    const std::string raise =
        want > 0 ? fmt("Raise Transmit latency to %g ms in Settings", want) : std::string("Raising Transmit latency will not cover it");
#if defined(_WIN32)
    const char* busy = "choose the High performance power plan, keep a laptop plugged in and close heavy programs";
#elif defined(__APPLE__)
    const char* busy = "keep a laptop plugged in (Low Power Mode slows audio threads) and close heavy programs";
#else
    const char* busy = "set the CPU governor to performance, check that virgild has real-time priority (see the "
                       "README) and close heavy programs";
#endif
    h.push_back(fmt("Virgil's sending thread woke up to %.1f ms late for the transmit latency of %.1f ms, so audio "
                    "to the network was dropped. %s; also %s.",
                    send_lag, tx_latency_ms_, raise.c_str(), busy));
  }
  if (sug_ready && sug.direction != TxLatencySuggestion::Direction::Keep && !(send_lag > 0))
    h.push_back(sug.text + " Set it in Settings; Dante receivers keep their own latency setting.");
  if (lost)
    h.push_back("Audio packets are being lost on the way. Use wired Ethernet (not Wi-Fi), check cables, and turn "
                "off Energy-Efficient Ethernet (EEE, \"green Ethernet\") on the switch ports.");
  for (const auto& a : apps)
    h.push_back(a + fmt(" did not keep up. Raise its buffer size, or raise Virgil's transmit latency (now %.1f ms).",
                        tx_latency_ms_));
  if (engine)
#if defined(_WIN32)
    h.push_back("The computer kept Virgil's audio engine waiting. Close heavy programs, choose the High "
                "performance (or Ultimate performance) power plan and keep a laptop plugged in. If it keeps "
                "happening, LatencyMon (free) shows which driver holds up the computer; Wi-Fi, Bluetooth and "
                "GPU drivers are common culprits.");
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
  j.kv("network_late_packets", (unsigned long long)near_late_packets_);  // close to the limit, still played
  j.kv("worst_stall_ms", worst_stall_ms_);
  j.kv("worst_send_wakeup_lag_ms", worst_wakeup_lag_ms_);
  j.key("send_wakeups_late").begin_obj();
  j.kv("over_0_5ms", (unsigned long long)send_lag_over_[0]).kv("over_1ms", (unsigned long long)send_lag_over_[1]);
  j.kv("over_2ms", (unsigned long long)send_lag_over_[2]).kv("over_4ms", (unsigned long long)send_lag_over_[3]);
  j.end_obj();
  j.kv("worst_send_lag_ms", worst_send_lag_ms_);
  {
    const TxLatencySuggestion t = tx_suggestion_locked();
    j.key("tx_latency_suggestion").begin_obj();
    j.kv("state", tx_suggestion_state_name(t.state));
    j.kv("observed_s", t.observed_s).kv("needed_s", t.needed_s);
    j.kv("tick_us", (unsigned long long)tx_obs_.tick_us);
    j.kv("current_ms", t.current_ms).kv("suggested_ms", t.suggested_ms).kv("min_ms", t.min_ms);
    j.kv("direction", tx_suggestion_direction_name(t.direction));
    j.kv("late_sends", (unsigned long long)t.late_sends).kv("worst_lag_ms", t.worst_lag_ms);
    j.kv("confidence", t.confidence).kv("text", t.text);
    j.end_obj();
  }
  j.key("hints").begin_arr();
  for (const auto& s : hints_locked(now)) j.v(s);
  j.end_arr();
  j.key("events").begin_arr();
  for (auto it = events_.rbegin(); it != events_.rend(); ++it) {  // newest first
    j.begin_obj();
    j.kv("start_ms", (unsigned long long)it->start_ms).kv("end_ms", (unsigned long long)it->end_ms);
    j.kv("kind", kind_name(it->kind)).kvb("dropout", is_dropout(it->kind));
    j.kv("severity", it->kind == Kind::ClockLost ? "error" : is_dropout(it->kind) ? "warn" : "info");
    j.kv("text", event_text(*it, period_ms_, rx_latency_ms_, tx_latency_ms_));
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
    r += fmt("  %-17s %" PRIu64 "\n", kind_name(t.first), t.second);
    total += t.second;
  }
  if (!total) r += "  no dropouts\n";
  r += fmt("  worst network packet age %.1f ms (receive latency %.1f ms)\n", worst_age_ms_, rx_latency_ms_);
  if (near_late_packets_)
    r += fmt("  %" PRIu64 " network %s older than the receive latency, still played (close to the limit)\n",
             near_late_packets_, plural(near_late_packets_, "packet", "packets"));
  if (worst_stall_ms_ > 0) r += fmt("  longest computer stall %.1f ms\n", worst_stall_ms_);
  if (worst_wakeup_lag_ms_ > 0 || send_lag_over_[0])
    r += fmt("  sending thread woke up late (not dropouts while under the transmit latency of %.1f ms): worst "
             "%.1f ms; over 0.5 ms %" PRIu64 ", over 1 ms %" PRIu64 ", over 2 ms %" PRIu64 ", over 4 ms %" PRIu64
             "\n",
             tx_latency_ms_, worst_wakeup_lag_ms_, send_lag_over_[0], send_lag_over_[1], send_lag_over_[2],
             send_lag_over_[3]);
  if (worst_send_lag_ms_ > 0)
    r += fmt("  worst late send %.1f ms (transmit latency %.1f ms)\n", worst_send_lag_ms_, tx_latency_ms_);
  {
    const TxLatencySuggestion t = tx_suggestion_locked();
    r += fmt("  transmit latency suggestion (tick %u us, %.0f s of sending, worst wakeup lag %.2f ms, %" PRIu64
             " late sends): %s\n",
             unsigned(tx_obs_.tick_us), t.observed_s, t.worst_lag_ms, t.late_sends, t.text.c_str());
  }
  const auto hints = hints_locked(now);
  if (!hints.empty()) {
    r += "\nSuggestions\n";
    for (const auto& s : hints) r += "  - " + s + "\n";
  }
  r += "\nEvents (oldest first)\n";
  for (const auto& e : events_) {
    r += "  " + clock_time(e.start_ms);
    if (e.end_ms - e.start_ms >= 1000) r += fmt(" (+%llds)", (long long)((e.end_ms - e.start_ms) / 1000));
    r += fmt("  [%s] ", kind_name(e.kind)) + event_text(e, period_ms_, rx_latency_ms_, tx_latency_ms_) + "\n";
  }
  if (events_.empty()) r += "  none\n";
  return r;
}

}  // namespace virgil
