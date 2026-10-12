#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace virgil {
enum LogLevel { kLogError = 0, kLogWarn = 1, kLogInfo = 2, kLogDebug = 3 };
extern int g_log_level;
// Most recent error-level message (shown in the control panel).
std::string last_error_message();
void clear_last_error();
// The most recent log lines (at most a few hundred), oldest first.
std::vector<std::string> recent_log_lines();
// stderr was reopened on this file (virgild --log): rotate it to <path>.1
// when it reaches 5 MB. Empty = stderr is not ours to rotate.
void set_log_file(const std::string& path);
void log_message(int level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
}  // namespace virgil

#define VIRGIL_LOG_ERROR(...) ::virgil::log_message(::virgil::kLogError, __VA_ARGS__)
#define VIRGIL_LOG_WARN(...) ::virgil::log_message(::virgil::kLogWarn, __VA_ARGS__)
#define VIRGIL_LOG_INFO(...) ::virgil::log_message(::virgil::kLogInfo, __VA_ARGS__)
#define VIRGIL_LOG_DEBUG(...) ::virgil::log_message(::virgil::kLogDebug, __VA_ARGS__)
