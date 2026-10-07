#pragma once

#include <cstdio>
#include <string>

namespace virgil {
enum LogLevel { kLogError = 0, kLogWarn = 1, kLogInfo = 2, kLogDebug = 3 };
extern int g_log_level;
// Most recent error-level message (shown in the control panel).
std::string last_error_message();
void clear_last_error();
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
