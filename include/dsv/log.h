#pragma once

#include <cstdio>
#include <string>

namespace dsv {
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
}  // namespace dsv

#define DSV_LOG_ERROR(...) ::dsv::log_message(::dsv::kLogError, __VA_ARGS__)
#define DSV_LOG_WARN(...) ::dsv::log_message(::dsv::kLogWarn, __VA_ARGS__)
#define DSV_LOG_INFO(...) ::dsv::log_message(::dsv::kLogInfo, __VA_ARGS__)
#define DSV_LOG_DEBUG(...) ::dsv::log_message(::dsv::kLogDebug, __VA_ARGS__)
