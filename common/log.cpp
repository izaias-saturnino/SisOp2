#include "log.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <strings.h>

namespace {

std::mutex log_mutex;
LogLevel min_level = LogLevel::Info;
std::string component_name;

const char* level_name(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
    }
    return "?";
}

} // namespace

void log_init(const std::string& component, LogLevel default_level)
{
    component_name = component;
    min_level = default_level;

    const char* env = std::getenv("SISOP_LOG");
    if (env == nullptr) {
        return;
    }
    if (strcasecmp(env, "debug") == 0) min_level = LogLevel::Debug;
    else if (strcasecmp(env, "info") == 0) min_level = LogLevel::Info;
    else if (strcasecmp(env, "warn") == 0) min_level = LogLevel::Warn;
    else if (strcasecmp(env, "error") == 0) min_level = LogLevel::Error;
}

bool log_enabled(LogLevel level)
{
    return level >= min_level;
}

void log_write(LogLevel level, const std::string& message)
{
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t seconds = system_clock::to_time_t(now);
    int millis = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);

    std::tm local{};
    localtime_r(&seconds, &local);
    char clock[16];
    std::strftime(clock, sizeof(clock), "%H:%M:%S", &local);

    std::lock_guard<std::mutex> lock(log_mutex);
    std::cerr << clock << '.' << std::setfill('0') << std::setw(3) << millis << ' '
              << component_name << ' ' << level_name(level) << ' ' << message << std::endl;
}
