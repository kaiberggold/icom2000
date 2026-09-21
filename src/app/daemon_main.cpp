// intercomd: the composition root. Every concrete type gets constructed
// here and wired together through the interfaces defined in each module;
// nothing above this file knows about libgpiod, ALSA, or Unix sockets, and
// nothing below it knows about the others. See docs/ARCHITECTURE.md.
#include "icom/config/config_file.hpp"
#include "icom/config/station_registry.hpp"
#include "icom/core/event_loop.hpp"
#include "icom/core/logger.hpp"
#include "icom/core/loop_thread.hpp"
#include "icom/core/signal_watcher.hpp"
#include "icom/gpio/digital_pin.hpp"
#include "icom/hw/bell_controller.hpp"
#include "icom/hw/pwm.hpp"
#include "icom/hw/status_led.hpp"
#include "icom/hw/tcm1171_controller.hpp"
#include "icom/audio/audio_engine.hpp"
#include "icom/ipc/control_server.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using icom::core::EventLoop;
using icom::core::LoopThread;
using icom::core::SignalWatcher;
using icom::gpio::Edge;
using icom::gpio::Level;
using icom::gpio::PinConfig;
using icom::hw::BellController;
using icom::hw::LineState;
using icom::hw::Pwm;
using icom::hw::StatusLed;
using icom::hw::Tcm1171Controller;
using icom::ipc::CommandResult;
using namespace std::chrono_literals;

namespace {

// Command *names* are matched case-insensitively by ControlServer itself;
// this covers the keyword *arguments* (BELL's ON/OFF/RING, LINE's STATUS)
// so `intercomctl bell ring` works as naturally from a shell as
// `intercomctl BELL RING` does.
std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

struct Options {
    // unset means "not given on the command line" -- resolved against the
    // config file, then a built-in default, once the config is loaded
    // (see main()). Keeping these as an explicit CLI override rather than
    // pre-seeding them with the built-in defaults is what lets the config
    // file's [daemon] section actually take effect when the flag isn't
    // passed at all.
    std::optional<std::string> socketPath;
    std::optional<std::string> gpioChip;
    std::string configPath = "/etc/icom2000.conf";
    std::string logLevelSpec; // empty: leave whatever ICOM_LOG set (or the built-in default)
    bool logConsole = false;
};

Options parseArgs(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            opts.socketPath = argv[++i];
        } else if (arg == "--gpio-chip" && i + 1 < argc) {
            opts.gpioChip = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            opts.configPath = argv[++i];
        } else if (arg == "--log-level" && i + 1 < argc) {
            opts.logLevelSpec = argv[++i];
        } else if (arg == "--log-console") {
            opts.logConsole = true;
        } else if (arg == "--help") {
            std::cout
                    << "usage: intercomd [--config PATH] [--socket PATH] [--gpio-chip NAME]\n"
                    << "                 [--log-level SPEC] [--log-console]\n"
                    << "  --config PATH   config file (default: /etc/icom2000.conf; see config/icom2000.conf\n"
                    << "                  in the repo for the shipped defaults and format). --socket and\n"
                    << "                  --gpio-chip, if given, override that file's [daemon] section.\n"
                    << "  SPEC: a default level and/or per-component overrides, e.g.\n"
                    << "        \"warn,gpio.mock=debug,ipc.control_server=debug\"\n"
                    << "        (also settable via the ICOM_LOG environment variable;\n"
                    << "        --log-level takes precedence when both are given)\n"
                    << "  --log-console   also print log messages to stderr, at the level(s) above\n"
                    << "                  (in addition to syslog, not instead of it) -- useful when\n"
                    << "                  running interactively or under a debugger; not needed for a\n"
                    << "                  systemd-managed run, since journalctl already has everything\n"
                    << "                  syslog gets\n";
            std::exit(0);
        }
    }
    return opts;
}

icom::core::Logger& log = icom::core::getLogger("app");

} // namespace

int main(int argc, char** argv) {

    icom::core::initSyslog("icom2000");
    log.info("Daemon started");

    const Options opts = parseArgs(argc, argv);

    if (opts.logConsole) {
        icom::core::setConsoleOutput(true);
    }

    // ICOM_LOG sets the baseline (e.g. from systemd's Environment=); a
    // --log-level on the command line overrides it for one run without
    // having to touch the unit file. Both go through the same parser, so
    // both fail the same way -- loudly, before anything else starts up --
    // on a typo rather than silently keeping whatever level components
    // happened to default to.
    if (!icom::core::configureLevelsFromEnv()) {
        std::cerr << "intercomd: invalid ICOM_LOG value\n";
        return 1;
    }
    if (!opts.logLevelSpec.empty() && !icom::core::configureLevels(opts.logLevelSpec)) {
        std::cerr << "intercomd: invalid --log-level value: " << opts.logLevelSpec << "\n";
        return 1;
    }

    // Single load point for every runtime-tunable value (GPIO lines,
    // station->device mapping, ...) -- see docs/ARCHITECTURE.md
    // "Configuration". A missing file is not an error (falls back to the
    // built-in defaults below, which match config/icom2000.conf exactly);
    // a malformed *existing* file is, since silently keeping stale
    // defaults there would be more confusing than refusing to start.
    const auto configResult = icom::config::File::load(opts.configPath);
    if (!configResult.ok) {
        std::cerr << "intercomd: " << opts.configPath << ": " << configResult.error << "\n";
        return 1;
    }
    const icom::config::File& config = configResult.config;
    if (!configResult.fileFound) {
        log.warn("no config file at " + opts.configPath + " -- using built-in defaults");
    }

    // BCM/line numbers below are placeholders for a sketch, not a
    // verified wiring diagram -- see docs/ARCHITECTURE.md "Pin
    // assignments" before touching real hardware. They're also the
    // fallback used when config/icom2000.conf's [gpio.*] sections are
    // absent, so they must stay in sync with that file.
    //
    // GPIO23/24/27 are NOT free to assign here: the HiFiBerry Codec Zero
    // HAT's own spec reserves them for its optional onboard status LEDs
    // (green=23, red=24) and tactile button (27) -- they're physically
    // wired on the HAT itself, not a software choice. ringModeLine/
    // hookDetectLine used to sit on 27/23 (a collision waiting to
    // happen once that HAT is actually populated); moved to 5/6 instead.
    unsigned bellPinLine = 17;
    unsigned ringModeLine = 5;
    unsigned polarityLine = 22;
    unsigned hookDetectLine = 6;
    unsigned statusLedLine = 23; // Codec Zero's own green status LED
    // The bell is driven via software PWM (icom::hw::Pwm), not a plain
    // digital on/off -- see docs/ARCHITECTURE.md "Software PWM". Default
    // duty == period (100%) reproduces the old plain-on/off behavior
    // exactly for anyone who hasn't tuned these; lower the duty to make
    // ring() strike/buzz gentler (and draw less current) instead.
    unsigned pwmPeriodMs = 10;
    unsigned pwmDutyMs = 10;
    try {
        bellPinLine = config.getUint("gpio.bell", "line", bellPinLine);
        pwmPeriodMs = config.getUint("gpio.bell", "pwm_period_ms", pwmPeriodMs);
        pwmDutyMs = config.getUint("gpio.bell", "pwm_duty_ms", pwmDutyMs);
        ringModeLine = config.getUint("gpio.tcm1171", "ring_mode_line", ringModeLine);
        polarityLine = config.getUint("gpio.tcm1171", "polarity_line", polarityLine);
        hookDetectLine = config.getUint("gpio.tcm1171", "hook_detect_line", hookDetectLine);
        statusLedLine = config.getUint("gpio.status_led", "line", statusLedLine);
    } catch (const std::exception& e) {
        std::cerr << "intercomd: " << opts.configPath << ": invalid GPIO line number (" << e.what()
                  << ")\n";
        return 1;
    }

    const std::string socketPath =
        opts.socketPath.value_or(config.get("daemon", "socket_path", "/run/icom2000.sock"));
    const std::string gpioChip = opts.gpioChip.value_or(config.get("daemon", "gpio_chip", "gpiochip0"));

    // The central station name -> device mapping (requirement: stations
    // as logical names, never "left"/"right" or a channel index). Nothing
    // consumes the device names yet beyond handing them to the (still
    // stubbed) audio engine below, but every station-aware component from
    // here on refers to "door"/"inside" by name, never by channel.
    const icom::config::StationRegistry stations(config);
    for (const auto& station : stations.all()) {
        log.info("station \"" + station.name + "\": capture=" + station.captureDevice +
                 ", playback=" + station.playbackDevice);
    }

    log.info("starting (socket=" + socketPath + ", gpio-chip=" + gpioChip +
             ", config=" + opts.configPath + ")");

    EventLoop loop;

    SignalWatcher signals(loop, {SIGINT, SIGTERM}, [&](int signo) {
        log.info("received signal " + std::to_string(signo) + ", shutting down");
        loop.stop();
    });

    // A second EventLoop, on its own thread, for timing-sensitive
    // periodic work that shouldn't have to wait its turn behind whatever
    // the main loop (`loop`, above) is doing -- the bell's PWM cycle and
    // the status LED's blink sequence both live here now. See
    // icom/core/loop_thread.hpp and docs/ARCHITECTURE.md "Software PWM".
    // Declared before anything that depends on it (bell/its Pwm below) so
    // it's the LAST thing destroyed at shutdown, not the first -- C++
    // destroys locals in reverse declaration order. Also declared after
    // SignalWatcher on purpose: SignalWatcher blocks SIGINT/SIGTERM via
    // sigprocmask() on THIS (the main) thread specifically, and a newly
    // created thread inherits its creator's signal mask at creation time
    // -- so backgroundLoop's own thread needs to come into existence
    // *after* that block is in place, or the kernel could just as easily
    // deliver the signal to backgroundLoop's thread instead, which
    // installs no handler for it and would take the default
    // (terminate-the-process) action.
    LoopThread backgroundLoop;

    auto gpioBackend = icom::gpio::makeDefaultBackend();

    BellController bell(
        std::make_unique<Pwm>(
            gpioBackend->requestOutput(PinConfig{gpioChip, bellPinLine, "icom2000-bell"}, Level::LOW),
            backgroundLoop.loop(), std::chrono::milliseconds(pwmPeriodMs)),
        std::chrono::milliseconds(pwmDutyMs));

    Tcm1171Controller line(
    Tcm1171Controller::Pins{
        gpioBackend->requestOutput(PinConfig{gpioChip, ringModeLine, "icom2000-tcm1171-rm"},
        Level::LOW),
        gpioBackend->requestOutput(PinConfig{gpioChip, polarityLine, "icom2000-tcm1171-fr"},
        Level::LOW),
        gpioBackend->requestInput(PinConfig{gpioChip, hookDetectLine, "icom2000-tcm1171-hook"},
        Edge::BOTH),
    },
    loop);

    StatusLed statusLed(gpioBackend->requestOutput(
                            PinConfig{gpioChip, statusLedLine, "icom2000-status-led"}, Level::LOW));

    auto audio = icom::audio::makeNullEngine(stations);
    audio->start();

    icom::ipc::ControlServer server(socketPath, loop);

    server.registerCommand("PING", [](const auto&) { return CommandResult::success("pong"); });

    server.registerCommand("BELL", [&](const std::vector<std::string>& args) {
        if (args.empty()) {
            return CommandResult::failure("usage: BELL ON|OFF|RING [MS]");
        }
        const std::string sub = toUpper(args[0]);
        if (sub == "ON") {
            bell.ring();
        } else if (sub == "OFF") {
            bell.silence();
        } else if (sub == "RING") {
            const int ms = args.size() > 1 ? std::atoi(args[1].c_str()) : 1000;
            bell.ringFor(std::chrono::milliseconds(ms), loop);
        } else {
            return CommandResult::failure("usage: BELL ON|OFF|RING [MS]");
        }
        return CommandResult::success(bell.isRinging() ? "ringing" : "silent");
    });

    server.registerCommand("LINE", [&](const std::vector<std::string>& args) {
        if (args.empty() || toUpper(args[0]) != "STATUS") {
            return CommandResult::failure("usage: LINE STATUS");
        }
        return CommandResult::success(icom::hw::toString(line.state()));
    });

    server.registerCommand("STATUS", [&](const auto&) {
        return CommandResult::success(std::string("line=") + icom::hw::toString(line.state()) +
                                      " bell=" + (bell.isRinging() ? "ringing" : "silent") +
                                      " audio=" + (audio->isRunning() ? "up" : "down"));
    });

    server.start();

    // Visual "the daemon is up and its main loop is about to start"
    // signal -- fire-and-forget, and now shares backgroundLoop with the
    // bell's PWM cycle rather than running on the main loop: post() is
    // the only backgroundLoop.loop() operation safe to call from here
    // (the main thread), so the actual blinkNTimes() call has to happen
    // inside the posted lambda, on that loop's own thread.
    backgroundLoop.loop().post([&statusLed, &backgroundLoop] {
        statusLed.blinkNTimes(3, backgroundLoop.loop(), 50ms, 50ms);
    });

    log.info("ready");
    loop.run();

    log.info("stopped");
    audio->stop();
    return 0;
}
