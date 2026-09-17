#include "icom/core/logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace icom::core {

namespace {

std::atomic<LogLevel> g_min_level{LogLevel::Info};

const char* level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

} // namespace

void set_min_log_level(LogLevel level) { g_min_level.store(level, std::memory_order_relaxed); }

void log(LogLevel level, std::string_view message) {
    if (level < g_min_level.load(std::memory_order_relaxed)) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&now_c, &tm_buf);

    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &tm_buf);

    // journald timestamps entries itself; this local HH:MM:SS is mainly for
    // reading raw stderr during development off-target.
    std::fprintf(stderr, "%s [%s] %.*s\n", timestamp, level_tag(level),
                 static_cast<int>(message.size()), message.data());
}

} // namespace icom::core
