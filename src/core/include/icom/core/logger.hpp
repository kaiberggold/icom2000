#pragma once

#include <string_view>

namespace icom::core {

// Deliberately tiny: writes leveled lines to stderr, which systemd/journald
// captures on their own. Swap for spdlog or similar later if a project
// actually needs structured/async logging -- not worth the dependency yet.
enum class LogLevel { Debug, Info, Warn, Error };

void set_min_log_level(LogLevel level);

void log(LogLevel level, std::string_view message);

inline void log_debug(std::string_view message) { log(LogLevel::Debug, message); }
inline void log_info(std::string_view message) { log(LogLevel::Info, message); }
inline void log_warn(std::string_view message) { log(LogLevel::Warn, message); }
inline void log_error(std::string_view message) { log(LogLevel::Error, message); }

} // namespace icom::core
