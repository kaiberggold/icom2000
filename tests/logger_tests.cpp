// Covers the pure logic in icom::core::Logger/get_logger/configure_levels:
// registry identity, level filtering, and configure_levels()'s parsing and
// "validate everything before applying anything" contract. Does NOT check
// that messages actually reach syslog -- that needs a real (or faked)
// /dev/log listener, which was verified manually against this build (see
// the commit message / docs/ARCHITECTURE.md "Logging"); it isn't something
// worth automating a fake Unix-datagram-socket receiver for in this suite.
//
// The logger registry is a process-wide singleton, so tests share it.
// Each test below either uses a component name nothing else in this binary
// touches, or explicitly sets the level(s) it depends on immediately
// before asserting -- never relies on whatever a previous test (or link
// order) left the *shared* default level at.
#include "check.hpp"

#include "icom/core/logger.hpp"

#include <cstdlib>
#include <sstream>

using namespace icom::core;

namespace {

void test_get_logger_returns_same_instance_for_same_name() {
    Logger& a = get_logger("test.logger.identity");
    Logger& b = get_logger("test.logger.identity");
    CHECK(&a == &b);
    CHECK(a.component() == "test.logger.identity");
}

void test_distinct_names_are_distinct_loggers() {
    Logger& a = get_logger("test.logger.distinct.a");
    Logger& b = get_logger("test.logger.distinct.b");
    CHECK(&a != &b);
}

void test_set_level_round_trips() {
    Logger& log = get_logger("test.logger.set_level");
    log.set_level(LogLevel::Error);
    CHECK(log.level() == LogLevel::Error);
    log.set_level(LogLevel::Debug);
    CHECK(log.level() == LogLevel::Debug);
}

void test_new_logger_starts_at_current_default() {
    set_default_level(LogLevel::Warn);
    Logger& log = get_logger("test.logger.fresh_at_default");
    CHECK(log.level() == LogLevel::Warn);
}

void test_configure_levels_default_applies_to_unlisted_existing_loggers() {
    Logger& untouched = get_logger("test.logger.configure.untouched");
    Logger& overridden = get_logger("test.logger.configure.overridden");
    untouched.set_level(LogLevel::Error);
    overridden.set_level(LogLevel::Error);

    const bool ok = configure_levels("warn,test.logger.configure.overridden=debug");
    CHECK(ok);

    // The bare default applies to every already-registered logger EXCEPT
    // the ones this same spec named explicitly.
    CHECK(untouched.level() == LogLevel::Warn);
    CHECK(overridden.level() == LogLevel::Debug);
}

void test_configure_levels_creates_named_component_if_new() {
    const bool ok = configure_levels("test.logger.configure.brand_new=error");
    CHECK(ok);
    CHECK(get_logger("test.logger.configure.brand_new").level() == LogLevel::Error);
}

void test_configure_levels_rejects_unknown_level_without_side_effects() {
    Logger& log = get_logger("test.logger.configure.reject_unknown");
    log.set_level(LogLevel::Info);

    CHECK(!configure_levels("test.logger.configure.reject_unknown=not-a-level"));
    CHECK(log.level() == LogLevel::Info); // unchanged -- validated before applied
}

void test_configure_levels_rejects_bad_default_without_side_effects() {
    Logger& log = get_logger("test.logger.configure.reject_bad_default");
    log.set_level(LogLevel::Info);

    CHECK(!configure_levels("not-a-level,test.logger.configure.reject_bad_default=debug"));
    CHECK(log.level() == LogLevel::Info); // whole spec rejected, override never applied either
}

void test_configure_levels_rejects_empty_component_name() {
    CHECK(!configure_levels("=debug"));
}

void test_configure_levels_rejects_multiple_bare_tokens() {
    CHECK(!configure_levels("info,warn"));
}

void test_configure_levels_level_names_are_case_insensitive() {
    Logger& log = get_logger("test.logger.configure.case_insensitive");
    CHECK(configure_levels("test.logger.configure.case_insensitive=DeBuG"));
    CHECK(log.level() == LogLevel::Debug);
}

void test_configure_levels_from_env_reads_named_variable() {
    Logger& log = get_logger("test.logger.configure.from_env");
    log.set_level(LogLevel::Error);

    ::setenv("ICOM2000_TEST_LOG_SPEC", "test.logger.configure.from_env=warn", 1);
    CHECK(configure_levels_from_env("ICOM2000_TEST_LOG_SPEC"));
    CHECK(log.level() == LogLevel::Warn);
    ::unsetenv("ICOM2000_TEST_LOG_SPEC");
}

void test_configure_levels_from_env_unset_is_a_harmless_noop() {
    ::unsetenv("ICOM2000_TEST_LOG_SPEC_UNSET");
    Logger& log = get_logger("test.logger.configure.from_env_unset");
    log.set_level(LogLevel::Error);

    CHECK(configure_levels_from_env("ICOM2000_TEST_LOG_SPEC_UNSET"));
    CHECK(log.level() == LogLevel::Error); // untouched
}

// set_console_output() is process-wide, like the level registry -- these
// leave it back off when done so they don't affect any test that runs
// after them (there's no ordering guarantee between translation units'
// test functions beyond what main() below imposes).
void test_console_output_off_by_default_writes_nothing_to_stderr() {
    Logger& log = get_logger("test.logger.console.off_by_default");
    log.set_level(LogLevel::Debug);

    std::ostringstream captured;
    std::streambuf* real_cerr = std::cerr.rdbuf(captured.rdbuf());
    log.info("should not appear");
    std::cerr.rdbuf(real_cerr);

    CHECK(captured.str().empty());
}

void test_console_output_when_enabled_writes_level_component_and_message() {
    Logger& log = get_logger("test.logger.console.enabled");
    log.set_level(LogLevel::Debug);
    set_console_output(true);

    std::ostringstream captured;
    std::streambuf* real_cerr = std::cerr.rdbuf(captured.rdbuf());
    log.warn("something happened");
    std::cerr.rdbuf(real_cerr);
    set_console_output(false);

    const std::string out = captured.str();
    CHECK(out.find("WARN") != std::string::npos);
    CHECK(out.find("test.logger.console.enabled") != std::string::npos);
    CHECK(out.find("something happened") != std::string::npos);
}

void test_console_output_still_respects_the_logger_level() {
    Logger& log = get_logger("test.logger.console.level_filtered");
    log.set_level(LogLevel::Error);
    set_console_output(true);

    std::ostringstream captured;
    std::streambuf* real_cerr = std::cerr.rdbuf(captured.rdbuf());
    log.debug("filtered out below the logger's own level");
    std::cerr.rdbuf(real_cerr);
    set_console_output(false);

    CHECK(captured.str().empty());
}

} // namespace

int main() {
    test_get_logger_returns_same_instance_for_same_name();
    test_distinct_names_are_distinct_loggers();
    test_set_level_round_trips();
    test_new_logger_starts_at_current_default();
    test_configure_levels_default_applies_to_unlisted_existing_loggers();
    test_configure_levels_creates_named_component_if_new();
    test_configure_levels_rejects_unknown_level_without_side_effects();
    test_configure_levels_rejects_bad_default_without_side_effects();
    test_configure_levels_rejects_empty_component_name();
    test_configure_levels_rejects_multiple_bare_tokens();
    test_configure_levels_level_names_are_case_insensitive();
    test_configure_levels_from_env_reads_named_variable();
    test_configure_levels_from_env_unset_is_a_harmless_noop();
    test_console_output_off_by_default_writes_nothing_to_stderr();
    test_console_output_when_enabled_writes_level_component_and_message();
    test_console_output_still_respects_the_logger_level();

    const int failures = icom::testing::failure_count();
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
