// Covers the pure logic in icom::core::Logger/getLogger/configureLevels:
// registry identity, level filtering, and configureLevels()'s parsing and
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

namespace
{

    void testGetLoggerReturnsSameInstanceForSameName()
    {
        Logger& a = getLogger("test.logger.identity");
        Logger& b = getLogger("test.logger.identity");
        CHECK(&a == &b);
        CHECK(a.component() == "test.logger.identity");
    }

    void testDistinctNamesAreDistinctLoggers()
    {
        Logger& a = getLogger("test.logger.distinct.a");
        Logger& b = getLogger("test.logger.distinct.b");
        CHECK(&a != &b);
    }

    void testSetLevelRoundTrips()
    {
        Logger& log = getLogger("test.logger.set_level");
        log.setLevel(LogLevel::ERROR);
        CHECK(log.level() == LogLevel::ERROR);
        log.setLevel(LogLevel::DEBUG);
        CHECK(log.level() == LogLevel::DEBUG);
    }

    void testNewLoggerStartsAtCurrentDefault()
    {
        setDefaultLevel(LogLevel::WARN);
        Logger& log = getLogger("test.logger.fresh_at_default");
        CHECK(log.level() == LogLevel::WARN);
    }

    void testConfigureLevelsDefaultAppliesToUnlistedExistingLoggers()
    {
        Logger& untouched = getLogger("test.logger.configure.untouched");
        Logger& overridden = getLogger("test.logger.configure.overridden");
        untouched.setLevel(LogLevel::ERROR);
        overridden.setLevel(LogLevel::ERROR);

        const bool ok = configureLevels("warn,test.logger.configure.overridden=debug");
        CHECK(ok);

        // The bare default applies to every already-registered logger EXCEPT
        // the ones this same spec named explicitly.
        CHECK(untouched.level() == LogLevel::WARN);
        CHECK(overridden.level() == LogLevel::DEBUG);
    }

    void testConfigureLevelsCreatesNamedComponentIfNew()
    {
        const bool ok = configureLevels("test.logger.configure.brand_new=error");
        CHECK(ok);
        CHECK(getLogger("test.logger.configure.brand_new").level() == LogLevel::ERROR);
    }

    void testConfigureLevelsRejectsUnknownLevelWithoutSideEffects()
    {
        Logger& log = getLogger("test.logger.configure.reject_unknown");
        log.setLevel(LogLevel::INFO);

        CHECK(!configureLevels("test.logger.configure.reject_unknown=not-a-level"));
        CHECK(log.level() == LogLevel::INFO); // unchanged -- validated before applied
    }

    void testConfigureLevelsRejectsBadDefaultWithoutSideEffects()
    {
        Logger& log = getLogger("test.logger.configure.reject_bad_default");
        log.setLevel(LogLevel::INFO);

        CHECK(!configureLevels("not-a-level,test.logger.configure.reject_bad_default=debug"));
        CHECK(log.level() == LogLevel::INFO); // whole spec rejected, override never applied either
    }

    void testConfigureLevelsRejectsEmptyComponentName()
    {
        CHECK(!configureLevels("=debug"));
    }

    void testConfigureLevelsRejectsMultipleBareTokens()
    {
        CHECK(!configureLevels("info,warn"));
    }

    void testConfigureLevelsLevelNamesAreCaseInsensitive()
    {
        Logger& log = getLogger("test.logger.configure.case_insensitive");
        CHECK(configureLevels("test.logger.configure.case_insensitive=DeBuG"));
        CHECK(log.level() == LogLevel::DEBUG);
    }

    void testConfigureLevelsFromEnvReadsNamedVariable()
    {
        Logger& log = getLogger("test.logger.configure.from_env");
        log.setLevel(LogLevel::ERROR);

        ::setenv("ICOM2000_TEST_LOG_SPEC", "test.logger.configure.from_env=warn", 1);
        CHECK(configureLevelsFromEnv("ICOM2000_TEST_LOG_SPEC"));
        CHECK(log.level() == LogLevel::WARN);
        ::unsetenv("ICOM2000_TEST_LOG_SPEC");
    }

    void testConfigureLevelsFromEnvUnsetIsAHarmlessNoop()
    {
        ::unsetenv("ICOM2000_TEST_LOG_SPEC_UNSET");
        Logger& log = getLogger("test.logger.configure.from_env_unset");
        log.setLevel(LogLevel::ERROR);

        CHECK(configureLevelsFromEnv("ICOM2000_TEST_LOG_SPEC_UNSET"));
        CHECK(log.level() == LogLevel::ERROR); // untouched
    }

// setConsoleOutput() is process-wide, like the level registry -- these
// leave it back off when done so they don't affect any test that runs
// after them (there's no ordering guarantee between translation units'
// test functions beyond what main() below imposes).
    void testConsoleOutputOffByDefaultWritesNothingToStderr()
    {
        Logger& log = getLogger("test.logger.console.off_by_default");
        log.setLevel(LogLevel::DEBUG);

        std::ostringstream captured;
        std::streambuf* realCerr = std::cerr.rdbuf(captured.rdbuf());
        log.info("should not appear");
        std::cerr.rdbuf(realCerr);

        CHECK(captured.str().empty());
    }

    void testConsoleOutputWhenEnabledWritesLevelComponentAndMessage()
    {
        Logger& log = getLogger("test.logger.console.enabled");
        log.setLevel(LogLevel::DEBUG);
        setConsoleOutput(true);

        std::ostringstream captured;
        std::streambuf* realCerr = std::cerr.rdbuf(captured.rdbuf());
        log.warn("something happened");
        std::cerr.rdbuf(realCerr);
        setConsoleOutput(false);

        const std::string out = captured.str();
        CHECK(out.find("WARN") != std::string::npos);
        CHECK(out.find("test.logger.console.enabled") != std::string::npos);
        CHECK(out.find("something happened") != std::string::npos);
    }

    void testConsoleOutputStillRespectsTheLoggerLevel()
    {
        Logger& log = getLogger("test.logger.console.level_filtered");
        log.setLevel(LogLevel::ERROR);
        setConsoleOutput(true);

        std::ostringstream captured;
        std::streambuf* realCerr = std::cerr.rdbuf(captured.rdbuf());
        log.debug("filtered out below the logger's own level");
        std::cerr.rdbuf(realCerr);
        setConsoleOutput(false);

        CHECK(captured.str().empty());
    }

} // namespace

int main()
{
    testGetLoggerReturnsSameInstanceForSameName();
    testDistinctNamesAreDistinctLoggers();
    testSetLevelRoundTrips();
    testNewLoggerStartsAtCurrentDefault();
    testConfigureLevelsDefaultAppliesToUnlistedExistingLoggers();
    testConfigureLevelsCreatesNamedComponentIfNew();
    testConfigureLevelsRejectsUnknownLevelWithoutSideEffects();
    testConfigureLevelsRejectsBadDefaultWithoutSideEffects();
    testConfigureLevelsRejectsEmptyComponentName();
    testConfigureLevelsRejectsMultipleBareTokens();
    testConfigureLevelsLevelNamesAreCaseInsensitive();
    testConfigureLevelsFromEnvReadsNamedVariable();
    testConfigureLevelsFromEnvUnsetIsAHarmlessNoop();
    testConsoleOutputOffByDefaultWritesNothingToStderr();
    testConsoleOutputWhenEnabledWritesLevelComponentAndMessage();
    testConsoleOutputStillRespectsTheLoggerLevel();

    const int failures = icom::testing::failureCount();
    if (failures > 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
