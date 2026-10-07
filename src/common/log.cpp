#include "dsv/log.h"

#include <cstdarg>
#include <ctime>
#include <mutex>

namespace dsv {

int g_log_level = kLogInfo;

namespace {
std::mutex g_err_mutex;
std::string g_last_error;
}  // namespace

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
  static std::mutex m;
  static const char* tags[] = {"error", "warn", "info", "debug"};
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  std::time_t t = std::time(nullptr);
  char ts[32];
  std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
  std::lock_guard<std::mutex> l(m);
  std::fprintf(stderr, "%s [%s] %s\n", ts, tags[level < 0 ? 0 : level > 3 ? 3 : level], msg);
}

}  // namespace dsv
