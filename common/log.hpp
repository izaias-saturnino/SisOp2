#pragma once

#include <sstream>
#include <string>

// Minimal thread-safe logger writing to stderr. The minimum level comes from
// the SISOP_LOG environment variable (debug, info, warn, error) and falls back
// to the default given to log_init().

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void log_init(const std::string& component, LogLevel default_level);
bool log_enabled(LogLevel level);
void log_write(LogLevel level, const std::string& message);

#define LOG_AT(level, expr)                          \
    do {                                             \
        if (log_enabled(level)) {                    \
            std::ostringstream log_stream_;          \
            log_stream_ << expr;                     \
            log_write(level, log_stream_.str());     \
        }                                            \
    } while (0)

#define LOG_DEBUG(expr) LOG_AT(LogLevel::Debug, expr)
#define LOG_INFO(expr) LOG_AT(LogLevel::Info, expr)
#define LOG_WARN(expr) LOG_AT(LogLevel::Warn, expr)
#define LOG_ERROR(expr) LOG_AT(LogLevel::Error, expr)
