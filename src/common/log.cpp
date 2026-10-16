#include "virgil/log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

namespace virgil {

int g_log_level = kLogInfo;

namespace {
// Never destroyed: Inferno's log thread can still log while the process
// exits (after main returned, static destructors had freed these and a late
// line wrote to the freed deque).
std::mutex& g_err_mutex = *new std::mutex;
std::string& g_last_error = *new std::string;
std::mutex& g_recent_mutex = *new std::mutex;
std::deque<std::string>& g_recent = *new std::deque<std::string>;  // for diagnostics reports
constexpr size_t kRecentLines = 400;

// Log file opened by the daemon itself (--log): rotated by size so it cannot
// grow without bound (the Windows service and the macOS launch daemon log
// this way). Guarded by g_write_mutex, like every write to stderr here.
std::mutex& g_write_mutex = *new std::mutex;
std::string& g_log_path = *new std::string;
long long g_log_bytes = 0;
constexpr long long kMaxLogBytes = 5ll * 1024 * 1024;

#if defined(_WIN32)
const char* const kNullDevice = "NUL";
#else
const char* const kNullDevice = "/dev/null";
#endif

// Caller holds g_write_mutex. Keeps one old file: <log>.1.
void rotate_log_locked() {
  std::fflush(stderr);
  // Close the file first (Windows cannot rename an open file).
  if (!std::freopen(kNullDevice, "w", stderr)) return;
  const std::string old = g_log_path + ".1";
  std::remove(old.c_str());
  std::rename(g_log_path.c_str(), old.c_str());
  if (!std::freopen(g_log_path.c_str(), "a", stderr)) {
    // Could not reopen: keep stderr usable and stop rotating.
    if (!std::freopen(kNullDevice, "w", stderr)) {
      // nothing more to do: logging is lost either way
    }
    g_log_path.clear();
    return;
  }
  std::setvbuf(stderr, nullptr, _IOLBF, 1024);
  g_log_bytes = 0;
}
}  // namespace

void set_log_file(const std::string& path) {
  std::lock_guard<std::mutex> l(g_write_mutex);
  g_log_path = path;
  g_log_bytes = 0;
  if (path.empty()) return;
  if (std::fseek(stderr, 0, SEEK_END) == 0) {
    const long pos = std::ftell(stderr);
    g_log_bytes = pos < 0 ? kMaxLogBytes : pos;
  }
  if (g_log_bytes >= kMaxLogBytes) rotate_log_locked();
}

std::vector<std::string> recent_log_lines() {
  std::lock_guard<std::mutex> l(g_recent_mutex);
  return std::vector<std::string>(g_recent.begin(), g_recent.end());
}

std::string last_error_message() {
  std::lock_guard<std::mutex> l(g_err_mutex);
  return g_last_error;
}

void clear_last_error() {
  std::lock_guard<std::mutex> l(g_err_mutex);
  g_last_error.clear();
}

void log_message(int level, const char* fmt, ...) {
  if (level == kLogError) {
    char m[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(g_err_mutex);
    g_last_error = m;
  }
  if (level > g_log_level) return;
  static const char* tags[] = {"error", "warn", "info", "debug"};
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  std::time_t t = std::time(nullptr);
  char ts[32];
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  std::strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
  const char* tag = tags[level < 0 ? 0 : level > 3 ? 3 : level];
  {
    std::lock_guard<std::mutex> l(g_recent_mutex);
    g_recent.push_back(std::string(ts) + " [" + tag + "] " + msg);
    if (g_recent.size() > kRecentLines) g_recent.pop_front();
  }
  std::lock_guard<std::mutex> l(g_write_mutex);
  const int n = std::fprintf(stderr, "%s [%s] %s\n", ts, tag, msg);
  // stderr may be redirected to a (buffered) file: never keep lines back,
  // or the last moments before a problem are missing from the log.
  std::fflush(stderr);
  if (!g_log_path.empty()) {
    if (n > 0) g_log_bytes += n;
    if (g_log_bytes >= kMaxLogBytes) rotate_log_locked();
  }
}

}  // namespace virgil
