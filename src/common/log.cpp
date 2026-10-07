#include "dsv/log.h"

#include <cstdarg>
#include <ctime>
#include <mutex>

namespace dsv {

int g_log_level = kLogInfo;

void log_message(int level, const char* fmt, ...) {
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
