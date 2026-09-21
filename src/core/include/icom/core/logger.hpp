#pragma once

#include <atomic>
#include <string>
#include <string_view>

#include <syslog.h>

namespace icom::core {

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

// A named, independently-leveled logger -- roughly one per module ("gpio",
// "gpio.mock", "hw.tcm1171", "ipc.control_server", ...). Get one via
// getLogger() (or a per-file reference to it, see any .cpp under src/ for
// the pattern); never construct one directly, so every component that
// exists is visible in one place (the registry) for whoever is tuning
// levels.
//
// Writes go to syslog(3) -- under systemd that lands in the journal
// (`journalctl`), same as any other well-behaved daemon; see
// docs/ARCHITECTURE.md "Logging" for why syslog rather than the kernel
// ring buffer (/dev/kmsg) despite the "kernel log" framing this started
// from. glibc's syslog() never throws or blocks the caller on failure
// (e.g. no /dev/log present) -- worst case a call here is a silent no-op,
// which is why nothing in this header reports an error for a failed log().
class Logger {
public:
    Logger(std::string component, LogLevel level);

    void setLevel(LogLevel level) { level_.store(level, std::memory_order_relaxed); }
    LogLevel level() const { return level_.load(std::memory_order_relaxed); }
    const std::string& component() const { return component_; }

    void log(LogLevel level, std::string_view message) const;

    void debug(std::string_view message) const { log(LogLevel::DEBUG, message); }
    void info(std::string_view message) const { log(LogLevel::INFO, message); }
    void warn(std::string_view message) const { log(LogLevel::WARN, message); }
    void error(std::string_view message) const { log(LogLevel::ERROR, message); }

private:
    std::string component_;
    std::atomic<LogLevel> level_;
};

// Returns the process-wide logger for `component`, creating it (at the
// current default level) on first use. Safe to call from a namespace-scope
// initializer in any translation unit, in any order relative to other
// translation units' initializers -- see logging.cpp for why that's
// exactly the guarantee this needs.
Logger& getLogger(std::string_view component);

// Sets the level newly-created loggers start at. Does not touch loggers
// that already exist; see configureLevels() for changing everything at
// once.
void setDefaultLevel(LogLevel level);

// Parses a spec like "warn,gpio=debug,ipc.control_server=debug" and
// applies it in one shot:
//   - a bare `level` token sets the default level (see
//     setDefaultLevel()) and is applied immediately to every
//     already-registered logger not otherwise named in this same spec;
//   - a `component=level` token sets that one logger's level, creating
//     the logger first if it doesn't exist yet.
// Whitespace around commas and `=` is ignored; level names are matched
// case-insensitively (component names are not -- they must match a
// registered logger's name exactly, e.g. "gpio.mock"). At most one bare
// token is allowed. Returns false -- and leaves every level unchanged --
// if the spec contains an unrecognized level name, an empty component
// name, or more than one bare token; the whole spec is validated before
// any of it is applied.
bool configureLevels(std::string_view spec);

// configureLevels() using the value of environment variable `envVar`
// (default ICOM_LOG). No-op if the variable is unset or empty. Returns
// false under the same conditions configureLevels() does.
bool configureLevelsFromEnv(const char* envVar = "ICOM_LOG");

// Mirrors every logged message to stderr, in addition to syslog, subject
// to the same per-component level filtering -- off by default. Meant for
// interactive use (e.g. a VS Code cppdbg session with
// "externalConsole": false, which captures the debuggee's stderr into the
// Debug Console) where waiting on `journalctl`/a syslog listener in a
// second window is more friction than it's worth. Safe to leave on for a
// real deployment too (stderr just goes wherever systemd sends it, which
// for a unit without its own `StandardError=` is the journal -- so this
// would only ever double up output that's already journal-bound), but
// there's no reason to bother when it's not being watched.
void setConsoleOutput(bool enable);

// Opens the syslog connection every Logger writes through (openlog(3)).
// Call once, early in main(), before spawning any other thread --
// openlog()'s `ident` is retained by pointer, not copied, by glibc, so
// this keeps its own copy alive for the process lifetime rather than
// trusting the caller's string to outlive every future log() call.
// Logging before this runs still works (glibc opens the connection
// lazily with default settings on first syslog() call); it just won't
// carry `ident`/`facility` yet.
void initSyslog(std::string_view ident, int facility = LOG_DAEMON);

} // namespace icom::core
