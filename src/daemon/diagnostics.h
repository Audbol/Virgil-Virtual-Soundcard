// Dropout diagnostics: turns the engine's counters (sampled once a second)
// into a log of events (late or lost network audio, apps that missed their
// deadline, a busy computer, clock trouble), per-minute history and hints.
// Survives engine restarts; shown by the control panel and Virgil Control.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "virgil/config.h"
#include "virgil/engine.h"

namespace virgil {

class Diagnostics {
 public:
  enum class Kind {
    NetLate,     // packets arrived after their receive latency
    NetLost,     // gaps in received streams
    AppLate,     // an app's audio came after its deadline (dropped)
    AppMissed,   // an app had not written its audio in time (silence sent)
    EngineLate,  // the engine woke up late (computer busy)
    NetSendLate, // the Dante sending thread woke up after the transmit latency (audio dropped)
    ClockJump,   // media clock realigned while running
    ClockLost,   // lost the clock leader
    ClockInfo,   // locked, leader changed (not a dropout)
    Service,     // engine (re)started, Dante stack restarted, settings saved
  };
  static bool is_dropout(Kind k) { return k <= Kind::ClockLost; }

  // Once a second while the engine runs. `cfg` is what it runs with.
  void sample(const EngineCounters& c, const Config& cfg, int64_t wall_ms);
  // A new engine (its counters start from zero).
  void engine_started(const Config& cfg, int64_t wall_ms);
  void note(Kind k, const std::string& text, int64_t wall_ms);
  void reset(int64_t wall_ms);

  // JSON for /api/diagnostics.
  std::string json(int64_t wall_ms) const;
  // Dropouts in the last `seconds`.
  uint64_t recent_dropouts(int64_t wall_ms, int64_t seconds = 3600) const;
  // Plain-text section of the support report (events, totals, hints).
  std::string report(int64_t wall_ms) const;

 private:
  struct Event {
    int64_t start_ms = 0, end_ms = 0;
    Kind kind = Kind::Service;
    std::string key;  // merges repeats (e.g. per app)
    std::string text;
    uint64_t count = 0;     // unit depends on the kind
    double value = 0;       // e.g. largest age / duration in ms
    uint64_t seconds = 0;   // seconds with occurrences
  };
  struct Minute {
    int64_t start_ms = 0;
    uint64_t net = 0, apps = 0, engine = 0, clock = 0;
  };
  struct AppPrev {
    uint64_t underrun_ticks = 0, late_frames = 0;
  };

  void add_locked(Kind k, const std::string& key, uint64_t count, double value, int64_t now,
                  const std::string& text);
  void bump_minute_locked(Kind k, int64_t now);
  std::vector<std::string> hints_locked(int64_t now) const;
  static const char* kind_name(Kind k);
  static std::string event_text(const Event& e, double period_ms, double rx_latency_ms, double tx_latency_ms);

  mutable std::mutex m_;
  int64_t since_ms_ = 0;
  int64_t grace_until_ = 0;
  bool have_prev_ = false;
  EngineCounters prev_;
  std::map<uint32_t, AppPrev> prev_apps_;  // by pid
  uint32_t prev_state_ = 0;
  std::string prev_gm_;
  double period_ms_ = 1, rx_latency_ms_ = 4, tx_latency_ms_ = 4;
  double worst_age_ms_ = 0;  // since reset
  double worst_send_lag_ms_ = 0;  // since reset
  std::deque<Event> events_;
  std::deque<Minute> minutes_;
  std::map<Kind, uint64_t> totals_;  // dropout events by kind
};

}  // namespace virgil
