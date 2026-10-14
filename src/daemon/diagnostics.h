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

// Transmit latency suggestion. The Dante sending thread sends frame f at
// media time f + send delay (two ticks + 0.5 ms) and stamps it f + transmit
// latency; the difference is the room it has to wake up late. From the worst
// wakeup lag seen while audio went out: send delay + worst lag + 1 ms,
// rounded up to 0.5 ms, between the smallest valid setting and 40 ms.
struct TxLatencyObservation {
  double observed_s = 0;     // seconds the transmit side ran (packets went out)
  uint32_t tick_us = 1000;   // tick the observation was made with
  double worst_lag_ms = 0;   // worst sending-thread wakeup lag
  uint64_t late_sends = 0;   // wakeups after the transmit latency (audio dropped)
  double worst_late_ms = 0;  // worst of those
  double current_ms = 4;     // transmit latency in use
};
struct TxLatencySuggestion {
  enum class State { Collecting, Ready };
  enum class Direction { Keep, Lower, Raise };
  State state = State::Collecting;
  Direction direction = Direction::Keep;
  double observed_s = 0, needed_s = 0;
  double current_ms = 0;
  double suggested_ms = 0;  // == current_ms unless Lower / Raise (0 while collecting)
  double min_ms = 0;        // smallest valid setting at this tick
  uint64_t late_sends = 0;
  double worst_lag_ms = 0;
  const char* confidence = "";  // "low" | "medium" | "high" ("" while collecting)
  std::string text;             // one sentence for the settings and the hints
};
constexpr double kTxSuggestNeededS = 30 * 60;  // only offered after this much sending
constexpr double kTxSuggestMarginMs = 1.0;     // on top of the worst lag
constexpr double kTxSuggestMaxMs = 40.0;       // validate_config's maximum
TxLatencySuggestion suggest_tx_latency(const TxLatencyObservation& o);
const char* tx_suggestion_state_name(TxLatencySuggestion::State s);       // "collecting" | "ready"
const char* tx_suggestion_direction_name(TxLatencySuggestion::Direction d);  // "keep" | "lower" | "raise"

class Diagnostics {
 public:
  enum class Kind {
    NetLate,     // received audio arrived after it was handed to the apps (lost)
    NetLost,     // gaps in received streams
    AppLate,     // an app's audio came after its deadline (dropped)
    AppMissed,   // an app had not written its audio in time (silence sent)
    EngineLate,  // the engine woke up too late (computer stalled): episodes
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
  // The transmit latency suggestion from what was observed since the last
  // reset (or the last change of tick_us).
  TxLatencySuggestion tx_latency_suggestion() const;

 private:
  struct Event {
    int64_t start_ms = 0, end_ms = 0;
    Kind kind = Kind::Service;
    std::string key;  // merges repeats (e.g. per app)
    std::string text;
    uint64_t count = 0;     // unit depends on the kind
    double value = 0;       // e.g. ms lost / largest lag in ms
    double extra = 0;       // NetLate: oldest packet (ms); kept as a maximum
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
                  const std::string& text, double extra = 0);
  void bump_minute_locked(Kind k, int64_t now);
  std::vector<std::string> hints_locked(int64_t now) const;
  TxLatencySuggestion tx_suggestion_locked() const;
  void reset_tx_observation_locked(uint32_t tick_us);
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
  uint64_t near_late_packets_ = 0;  // older than the receive latency, still played (since reset)
  double worst_stall_ms_ = 0;  // since reset
  double worst_wakeup_lag_ms_ = 0;  // sending thread, late or not (since reset)
  uint64_t send_lag_over_[4] = {};  // wakeups later than 0.5 / 1 / 2 / 4 ms (since reset)
  int64_t last_stall_ms_ = 0;  // an engine stall was seen in this sample (wall ms)
  double worst_send_lag_ms_ = 0;  // since reset
  // Transmit latency suggestion: what the sending thread did while packets
  // went out, since reset or the last change of tick_us (the send delay
  // depends on it). Engine reloads and other settings changes keep it; so
  // does changing the transmit latency (the lag does not depend on it).
  TxLatencyObservation tx_obs_;
  uint32_t tx_tick_us_ = 1000;  // tick of the engine running now
  std::deque<Event> events_;
  std::deque<Minute> minutes_;
  std::map<Kind, uint64_t> totals_;  // dropout events by kind
};

}  // namespace virgil
