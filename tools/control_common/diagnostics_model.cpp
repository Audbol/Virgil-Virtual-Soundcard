#include "diagnostics_model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>

#include "api.h"

namespace vc {

const char* diag_category_label(DiagCategory c) {
  switch (c) {
    case DiagCategory::Network: return "network";
    case DiagCategory::Apps: return "apps";
    case DiagCategory::Computer: return "computer busy";
    case DiagCategory::Clock: return "clock";
  }
  return "";
}

unsigned diag_category_color(DiagCategory c) {
  switch (c) {
    case DiagCategory::Network: return 0x6B9BF5;
    case DiagCategory::Apps: return 0xD9A93F;
    case DiagCategory::Computer: return 0xE07A2E;
    case DiagCategory::Clock: return 0xD2553F;
  }
  return 0x8C9097;
}

bool diag_kind_category(const std::string& kind, DiagCategory* out) {
  DiagCategory c;
  if (kind == "network-late" || kind == "network-lost") c = DiagCategory::Network;
  else if (kind == "app-late" || kind == "app-missed") c = DiagCategory::Apps;
  else if (kind == "engine-late" || kind == "network-send-late") c = DiagCategory::Computer;
  else if (kind == "clock-jump" || kind == "clock-lost") c = DiagCategory::Clock;
  else return false;
  if (out) *out = c;
  return true;
}

int64_t wall_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

namespace {

bool local_tm(int64_t ms, std::tm* out) {
  const std::time_t t = std::time_t(ms / 1000);
#if defined(_WIN32)
  return localtime_s(out, &t) == 0;
#else
  return localtime_r(&t, out) != nullptr;
#endif
}

// As the panel showed times: the time of day for today, else date and time.
std::string day_time(int64_t ms, int64_t now_ms) {
  std::tm a{}, b{};
  const bool today = local_tm(ms, &a) && local_tm(now_ms, &b) && a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
  return format_local_time(ms, today ? "%H:%M:%S" : "%Y-%m-%d %H:%M:%S");
}

Severity severity_of(const std::string& s) {
  if (s == "error") return Severity::Error;
  if (s == "warn") return Severity::Warn;
  return Severity::Info;
}

int64_t ms_of(const Json& j) { return int64_t(j.num()); }

}  // namespace

std::string format_local_time(int64_t wall_ms, const char* f) {
  std::tm tmv{};
  if (!local_tm(wall_ms, &tmv)) return {};
  char b[64];
  const size_t n = std::strftime(b, sizeof b, f, &tmv);
  return std::string(b, n);
}

bool parse_diagnostics(const std::string& body, Diagnostics* out) {
  Json j;
  if (!parse_json(body, &j) || j.type != Json::Object) return false;
  Diagnostics d;
  d.now_ms = ms_of(j["now_ms"]);
  if (!d.now_ms) d.now_ms = wall_now_ms();
  d.since_ms = ms_of(j["since_ms"]);
  d.dropouts_last_hour = (unsigned long long)std::max(0.0, j["dropouts_last_hour"].num());
  d.dropouts_total = (unsigned long long)std::max(0.0, j["dropouts_total"].num());
  d.rx_latency_ms = j["rx_latency_ms"].num();
  d.tx_latency_ms = j["tx_latency_ms"].num();

  const unsigned long long n = d.dropouts_last_hour;
  d.headline_severity = n ? Severity::Warn : Severity::Ok;
  d.headline = n ? std::to_string(n) + (n == 1 ? " dropout" : " dropouts") + " in the last hour"
                 : "No dropouts in the last hour";
  if (d.since_ms) d.since_text = "since " + day_time(d.since_ms, d.now_ms);
  if (d.dropouts_total)
    d.since_text += std::string(d.since_text.empty() ? "" : " \xC2\xB7 ") + std::to_string(d.dropouts_total) +
                    " in total";

  const Json& totals = j["totals"];
  for (const auto& kv : totals.o) {
    DiagCategory c;
    if (diag_kind_category(kv.first, &c)) d.totals[int(c)] += (unsigned long long)std::max(0.0, kv.second.num());
  }

  // The last 60 minutes, oldest first; minutes without trouble are absent.
  std::map<int64_t, const Json*> by_min;
  const Json& mins = j["minutes"];
  for (size_t i = 0; i < mins.size(); ++i) by_min[ms_of(mins[i]["t"])] = &mins[i];
  const int64_t m0 = d.now_ms - d.now_ms % 60000;
  static const char* const kKeys[kDiagCategories] = {"network", "apps", "engine", "clock"};
  for (int k = kDiagMinutes - 1; k >= 0; --k) {
    DiagMinute m;
    m.t_ms = m0 - int64_t(k) * 60000;
    auto it = by_min.find(m.t_ms);
    if (it != by_min.end())
      for (int c = 0; c < kDiagCategories; ++c) m.counts[c] = unsigned(std::max(0.0, (*it->second)[kKeys[c]].num()));
    m.label = format_local_time(m.t_ms, "%H:%M");
    m.tooltip = m.label + ": " + std::to_string(m.counts[0]) + " network, " + std::to_string(m.counts[1]) +
                " apps, " + std::to_string(m.counts[2]) + " busy, " + std::to_string(m.counts[3]) +
                " clock (seconds with trouble)";
    d.peak = std::max(d.peak, m.total());
    d.minutes.push_back(std::move(m));
  }

  const Json& hints = j["hints"];
  for (size_t i = 0; i < hints.size(); ++i) d.hints.push_back(valid_utf8(hints[i].str()));

  const Json& ev = j["events"];
  for (size_t i = 0; i < ev.size(); ++i) {
    const Json& e = ev[i];
    DiagEvent x;
    x.start_ms = ms_of(e["start_ms"]);
    x.end_ms = ms_of(e["end_ms"]);
    x.kind = e["kind"].str();
    x.dropout = e["dropout"].boolean();
    x.severity = severity_of(e["severity"].str());
    x.time_text = day_time(x.start_ms, d.now_ms);
    if (x.end_ms - x.start_ms >= 1000)
      x.duration_text = std::to_string((long long)std::llround(double(x.end_ms - x.start_ms) / 1000.0)) + " s";
    x.text = valid_utf8(e["text"].str());
    d.events.push_back(std::move(x));
  }
  *out = std::move(d);
  return true;
}

bool fetch_diagnostics(unsigned port, Diagnostics* out, std::string* error) {
  const HttpResult r = http(port, "GET", "/api/diagnostics");
  if (r.status != 200) {
    if (error) *error = http_error(r);
    return false;
  }
  if (!parse_diagnostics(r.body, out)) {
    if (error) *error = "The Virgil service sent diagnostics this version of Virgil Control cannot read.";
    return false;
  }
  return true;
}

bool fetch_report(unsigned port, std::string* text, std::string* error) {
  const HttpResult r = http(port, "GET", "/api/diagnostics/report");
  if (r.status != 200) {
    if (error) *error = http_error(r);
    return false;
  }
  if (text) *text = r.body;
  return true;
}

bool reset_diagnostics(unsigned port, std::string* error) {
  const HttpResult r = http(port, "POST", "/api/diagnostics/reset");
  if (r.status == 200) return true;
  if (error) *error = http_error(r);
  return false;
}

std::string default_report_file_name(int64_t wall_ms) {
  return "virgil-report-" + format_local_time(wall_ms ? wall_ms : wall_now_ms(), "%Y%m%d-%H%M") + ".txt";
}

std::vector<DiagBar> diag_chart_layout(const Diagnostics& d, float w, float h, float gap) {
  std::vector<DiagBar> out;
  const int n = int(d.minutes.size());
  if (n == 0 || w <= 0 || h <= 0) return out;
  float bw = (w - gap * float(n - 1)) / float(n);
  if (bw < 1) {
    gap = 0;
    bw = w / float(n);
  }
  const float peak = float(std::max(1u, d.peak));
  for (int i = 0; i < n; ++i) {
    const DiagMinute& m = d.minutes[size_t(i)];
    float bottom = h;
    for (int c = 0; c < kDiagCategories; ++c) {
      if (!m.counts[c]) continue;
      const float sh = float(m.counts[c]) / peak * h;
      DiagBar b;
      b.minute = i;
      b.category = DiagCategory(c);
      b.x = float(i) * (bw + gap);
      b.w = bw;
      b.h = sh;
      b.y = bottom - sh;
      bottom -= sh;
      out.push_back(b);
    }
  }
  return out;
}

int diag_chart_minute_at(float x, float w) {
  if (w <= 0 || x < 0 || x >= w) return -1;
  const int i = int(x / (w / float(kDiagMinutes)));
  return i < 0 ? -1 : i >= kDiagMinutes ? kDiagMinutes - 1 : i;
}

}  // namespace vc
