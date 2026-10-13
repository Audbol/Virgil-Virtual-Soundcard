// Virgil Control (all platforms): the "Dropouts and diagnostics" window's
// content, read from virgild's /api/diagnostics, ready to draw: headline,
// "since" line, the last hour per minute by category, hints and events.
// Times are formatted here in local time, so every app shows the same text.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vc {

enum class Severity { Ok, Info, Warn, Error };

// Dropout categories of the per-minute chart, bottom to top.
enum class DiagCategory { Network = 0, Apps = 1, Computer = 2, Clock = 3 };
constexpr int kDiagCategories = 4;
// Legend text: "network", "apps", "computer busy", "clock".
const char* diag_category_label(DiagCategory c);
// Bar colour as 0xRRGGBB on the apps' dark background (network blue, apps
// amber, computer busy ember, clock red, as in the old control panel).
unsigned diag_category_color(DiagCategory c);
// Category of a daemon event / totals kind ("network-late", "app-missed",
// ...); false for kinds that are not dropouts ("clock", "service").
bool diag_kind_category(const std::string& kind, DiagCategory* out);

constexpr int kDiagMinutes = 60;  // chart: the last 60 minutes
constexpr size_t kDiagShownEvents = 100;  // the panel listed at most this many

struct DiagMinute {
  int64_t t_ms = 0;  // start of the minute (wall clock, ms since 1970)
  unsigned counts[kDiagCategories] = {};  // seconds with trouble, per category
  unsigned total() const { return counts[0] + counts[1] + counts[2] + counts[3]; }
  std::string label;    // "14:05"
  std::string tooltip;  // "14:05: 1 network, 0 apps, 0 busy, 0 clock (seconds with trouble)"
};

struct DiagEvent {
  int64_t start_ms = 0, end_ms = 0;
  std::string kind;       // daemon kind name, e.g. "network-late", "service"
  bool dropout = false;
  Severity severity = Severity::Info;  // Warn (dropout), Error (clock lost), Info
  std::string time_text;  // start: "14:05:12" today, else "2026-10-12 14:05:12"
  std::string duration_text;  // "12 s" when it lasted a second or more, else ""
  std::string text;       // what happened, one sentence
};

struct Diagnostics {
  int64_t now_ms = 0, since_ms = 0;
  unsigned long long dropouts_last_hour = 0, dropouts_total = 0;
  // "No dropouts in the last hour" (Ok) / "3 dropouts in the last hour" (Warn).
  std::string headline;
  Severity headline_severity = Severity::Ok;
  // "since 14:02:11 · 12 in total" (the time as in DiagEvent::time_text).
  std::string since_text;
  // Dropout events since `since_ms`, per category (from the daemon's totals).
  unsigned long long totals[kDiagCategories] = {};
  // Exactly kDiagMinutes entries, oldest first; the last is the current minute.
  std::vector<DiagMinute> minutes;
  unsigned peak = 1;  // largest DiagMinute::total(), at least 1 (chart scale)
  std::vector<std::string> hints;  // what to do about it, one paragraph each
  std::vector<DiagEvent> events;   // newest first (up to 300; show kDiagShownEvents)
  double rx_latency_ms = 0, tx_latency_ms = 0;
};

// Parses a /api/diagnostics body. Local time for the texts.
bool parse_diagnostics(const std::string& json, Diagnostics* out);

// GET /api/diagnostics. On failure `error` holds a message for the user.
bool fetch_diagnostics(unsigned port, Diagnostics* out, std::string* error);
// GET /api/diagnostics/report: the plain-text report to save to a file.
bool fetch_report(unsigned port, std::string* text, std::string* error);
// POST /api/diagnostics/reset ("Clear").
bool reset_diagnostics(unsigned port, std::string* error);
// Suggested file name for the saved report: "virgil-report-20261013-1405.txt"
// (local time of `wall_ms`; 0 = now).
std::string default_report_file_name(int64_t wall_ms = 0);

// Chart geometry: one rectangle per non-empty segment of the stacked bars,
// for a chart area `w` x `h` (origin top left, y down). Bars are spaced
// `gap` apart, oldest left; segments stack Network at the bottom up to Clock.
struct DiagBar {
  int minute = 0;  // index into Diagnostics::minutes
  DiagCategory category = DiagCategory::Network;
  float x = 0, y = 0, w = 0, h = 0;
};
std::vector<DiagBar> diag_chart_layout(const Diagnostics& d, float w, float h, float gap = 2);
// Index of the minute under x (for the tooltip), or -1.
int diag_chart_minute_at(float x, float w);

// Local time helpers used above ("%H:%M:%S" etc.).
std::string format_local_time(int64_t wall_ms, const char* strftime_format);
int64_t wall_now_ms();

}  // namespace vc
