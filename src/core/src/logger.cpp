#include "icom/core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace icom::core {

namespace {

int to_syslog_priority(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return LOG_DEBUG;
        case LogLevel::Info:  return LOG_INFO;
        case LogLevel::Warn:  return LOG_WARNING;
        case LogLevel::Error: return LOG_ERR;
    }
    return LOG_INFO;
}

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

// Set via set_console_output(); read on every Logger::log() call, so a
// plain std::atomic rather than something guarded by Registry::mutex.
std::atomic<bool> console_output_enabled{false};

std::string_view trim(std::string_view s) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<LogLevel> parse_level(std::string_view text) {
    std::string lower(trim(text));
    std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    if (lower == "debug") return LogLevel::Debug;
    if (lower == "info") return LogLevel::Info;
    if (lower == "warn" || lower == "warning") return LogLevel::Warn;
    if (lower == "error" || lower == "err") return LogLevel::Error;
    return std::nullopt;
}

// Function-local static, not a plain namespace-scope global: the order in
// which different translation units' namespace-scope variables run their
// dynamic initializers is unspecified, and several .cpp files declare a
// namespace-scope `Logger& kLog = get_logger("...")` that runs during
// exactly that phase (see e.g. src/core/src/event_loop.cpp). If the
// registry were itself a plain global, whichever TU's initializer
// happened to run first could read it before it was constructed. A
// function-local static is guaranteed to be initialized on its first use
// no matter which TU that first use comes from, which is the guarantee
// get_logger() promises its callers in logger.hpp.
struct Registry {
    std::mutex mutex;
    std::unordered_map<std::string, Logger> loggers;
    std::atomic<LogLevel> default_level{LogLevel::Info};
};

Registry& registry() {
    static Registry instance;
    return instance;
}

} // namespace

Logger::Logger(std::string component, LogLevel level)
    : component_(std::move(component)), level_(level) {}

void Logger::log(LogLevel level, std::string_view message) const {
    if (level < level_.load(std::memory_order_relaxed)) {
        return;
    }
    ::syslog(to_syslog_priority(level), "[%s] %.*s", component_.c_str(),
             static_cast<int>(message.size()), message.data());
    if (console_output_enabled.load(std::memory_order_relaxed)) {
        // Built as one string and written with a single `<<` (std::endl to
        // flush immediately, so it shows up in a VS Code Debug Console
        // right away rather than sitting in a buffer) -- std::cerr flushes
        // after every individual `<<` by default, so writing the pieces
        // separately would let concurrent loggers' lines interleave.
        std::cerr << (std::string(level_name(level)) + " [" + component_ + "] " + std::string(message))
                   << std::endl;
    }
}

Logger& get_logger(std::string_view component) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    auto it = r.loggers.find(std::string(component));
    if (it == r.loggers.end()) {
        it = r.loggers
                 .try_emplace(std::string(component), std::string(component),
                              r.default_level.load(std::memory_order_relaxed))
                 .first;
    }
    return it->second;
}

void set_default_level(LogLevel level) {
    registry().default_level.store(level, std::memory_order_relaxed);
}

bool configure_levels(std::string_view spec) {
    std::optional<LogLevel> default_level;
    std::vector<std::pair<std::string, LogLevel>> overrides;

    std::size_t pos = 0;
    while (pos <= spec.size()) {
        const std::size_t comma = spec.find(',', pos);
        const std::string_view token = trim(spec.substr(pos, comma == std::string_view::npos
                                                                  ? std::string_view::npos
                                                                  : comma - pos));
        pos = (comma == std::string_view::npos) ? spec.size() + 1 : comma + 1;

        if (token.empty()) {
            continue;
        }

        const std::size_t eq = token.find('=');
        if (eq == std::string_view::npos) {
            if (default_level.has_value()) {
                return false; // more than one bare level token
            }
            const auto level = parse_level(token);
            if (!level) {
                return false;
            }
            default_level = level;
        } else {
            const std::string_view name = trim(token.substr(0, eq));
            const auto level = parse_level(token.substr(eq + 1));
            if (name.empty() || !level) {
                return false;
            }
            overrides.emplace_back(std::string(name), *level);
        }
    }

    Registry& r = registry();

    if (default_level) {
        r.default_level.store(*default_level, std::memory_order_relaxed);
        std::lock_guard lock(r.mutex);
        for (auto& [name, logger] : r.loggers) {
            const bool has_override =
                std::any_of(overrides.begin(), overrides.end(),
                            [&](const auto& o) { return o.first == name; });
            if (!has_override) {
                logger.set_level(*default_level);
            }
        }
    }

    for (const auto& [name, level] : overrides) {
        get_logger(name).set_level(level);
    }

    return true;
}

bool configure_levels_from_env(const char* env_var) {
    const char* value = std::getenv(env_var);
    if (value == nullptr || *value == '\0') {
        return true; // unset is not an error, just "nothing to configure"
    }
    return configure_levels(value);
}

void set_console_output(bool enable) {
    console_output_enabled.store(enable, std::memory_order_relaxed);
}

void init_syslog(std::string_view ident, int facility) {
    // openlog(3) keeps the `ident` pointer, it does not copy the string --
    // this static gives it something that outlives every future syslog()
    // call instead of trusting whatever the caller passed in to still be
    // alive then.
    static std::string ident_storage;
    ident_storage.assign(ident);
    ::openlog(ident_storage.c_str(), LOG_PID | LOG_NDELAY, facility);
}

} // namespace icom::core
