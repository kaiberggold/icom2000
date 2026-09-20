// intercomd: the composition root. Every concrete type gets constructed
// here and wired together through the interfaces defined in each module;
// nothing above this file knows about libgpiod, ALSA, or Unix sockets, and
// nothing below it knows about the others. See docs/ARCHITECTURE.md.
#include "icom/config/config_file.hpp"
#include "icom/config/station_registry.hpp"
#include "icom/core/event_loop.hpp"
#include "icom/core/logger.hpp"
#include "icom/core/signal_watcher.hpp"
#include "icom/gpio/digital_pin.hpp"
#include "icom/hw/bell_controller.hpp"
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
using icom::core::SignalWatcher;
using icom::gpio::Edge;
using icom::gpio::Level;
using icom::gpio::PinConfig;
using icom::hw::BellController;
using icom::hw::LineState;
using icom::hw::StatusLed;
using icom::hw::Tcm1171Controller;
using icom::ipc::CommandResult;

namespace {

// Command *names* are matched case-insensitively by ControlServer itself;
// this covers the keyword *arguments* (BELL's ON/OFF/RING, LINE's STATUS)
// so `intercomctl bell ring` works as naturally from a shell as
// `intercomctl BELL RING` does.
std::string to_upper(std::string s) {
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
    std::optional<std::string> socket_path;
    std::optional<std::string> gpio_chip;
    std::string config_path = "/etc/icom2000.conf";
    std::string log_level_spec; // empty: leave whatever ICOM_LOG set (or the built-in default)
    bool log_console = false;
};

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            opts.socket_path = argv[++i];
        } else if (arg == "--gpio-chip" && i + 1 < argc) {
            opts.gpio_chip = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            opts.config_path = argv[++i];
        } else if (arg == "--log-level" && i + 1 < argc) {
            opts.log_level_spec = argv[++i];
        } else if (arg == "--log-console") {
            opts.log_console = true;
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

icom::core::Logger& kLog = icom::core::get_logger("app");

} // namespace

int main(int argc, char** argv) {

    icom::core::init_syslog("icom2000");
     kLog.info("Daemon started");
    
    const Options opts = parse_args(argc, argv);

    if (opts.log_console) {
        icom::core::set_console_output(true);
    }

    // ICOM_LOG sets the baseline (e.g. from systemd's Environment=); a
    // --log-level on the command line overrides it for one run without
    // having to touch the unit file. Both go through the same parser, so
    // both fail the same way -- loudly, before anything else starts up --
    // on a typo rather than silently keeping whatever level components
    // happened to default to.
    if (!icom::core::configure_levels_from_env()) {
        std::cerr << "intercomd: invalid ICOM_LOG value\n";
        return 1;
    }
    if (!opts.log_level_spec.empty() && !icom::core::configure_levels(opts.log_level_spec)) {
        std::cerr << "intercomd: invalid --log-level value: " << opts.log_level_spec << "\n";
        return 1;
    }

    // Single load point for every runtime-tunable value (GPIO lines,
    // station->device mapping, ...) -- see docs/ARCHITECTURE.md
    // "Configuration". A missing file is not an error (falls back to the
    // built-in defaults below, which match config/icom2000.conf exactly);
    // a malformed *existing* file is, since silently keeping stale
    // defaults there would be more confusing than refusing to start.
    const auto config_result = icom::config::ConfigFile::load(opts.config_path);
    if (!config_result.ok) {
        std::cerr << "intercomd: " << opts.config_path << ": " << config_result.error << "\n";
        return 1;
    }
    const icom::config::ConfigFile& config = config_result.config;
    if (!config_result.file_found) {
        kLog.warn("no config file at " + opts.config_path + " -- using built-in defaults");
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
    // wired on the HAT itself, not a software choice. ring_mode_line/
    // hook_detect_line used to sit on 27/23 (a collision waiting to
    // happen once that HAT is actually populated); moved to 5/6 instead.
    unsigned bell_pin_line = 17;
    unsigned ring_mode_line = 5;
    unsigned polarity_line = 22;
    unsigned hook_detect_line = 6;
    unsigned status_led_line = 23; // Codec Zero's own green status LED
    try {
        bell_pin_line = config.get_uint("gpio.bell", "line", bell_pin_line);
        ring_mode_line = config.get_uint("gpio.tcm1171", "ring_mode_line", ring_mode_line);
        polarity_line = config.get_uint("gpio.tcm1171", "polarity_line", polarity_line);
        hook_detect_line = config.get_uint("gpio.tcm1171", "hook_detect_line", hook_detect_line);
        status_led_line = config.get_uint("gpio.status_led", "line", status_led_line);
    } catch (const std::exception& e) {
        std::cerr << "intercomd: " << opts.config_path << ": invalid GPIO line number (" << e.what()
                   << ")\n";
        return 1;
    }

    const std::string socket_path =
        opts.socket_path.value_or(config.get("daemon", "socket_path", "/run/icom2000.sock"));
    const std::string gpio_chip = opts.gpio_chip.value_or(config.get("daemon", "gpio_chip", "gpiochip0"));

    // The central station name -> device mapping (requirement: stations
    // as logical names, never "left"/"right" or a channel index). Nothing
    // consumes the device names yet beyond handing them to the (still
    // stubbed) audio engine below, but every station-aware component from
    // here on refers to "door"/"inside" by name, never by channel.
    const icom::config::StationRegistry stations(config);
    for (const auto& station : stations.all()) {
        kLog.info("station \"" + station.name + "\": capture=" + station.capture_device +
                   ", playback=" + station.playback_device);
    }

    kLog.info("starting (socket=" + socket_path + ", gpio-chip=" + gpio_chip +
               ", config=" + opts.config_path + ")");

    EventLoop loop;

    SignalWatcher signals(loop, {SIGINT, SIGTERM}, [&](int signo) {
        kLog.info("received signal " + std::to_string(signo) + ", shutting down");
        loop.stop();
    });

    auto gpio_backend = icom::gpio::make_default_backend();

    BellController bell(
        gpio_backend->request_output(PinConfig{gpio_chip, bell_pin_line, "icom2000-bell"}, Level::Low));

    Tcm1171Controller line(
        Tcm1171Controller::Pins{
            gpio_backend->request_output(PinConfig{gpio_chip, ring_mode_line, "icom2000-tcm1171-rm"},
                                          Level::Low),
            gpio_backend->request_output(PinConfig{gpio_chip, polarity_line, "icom2000-tcm1171-fr"},
                                          Level::Low),
            gpio_backend->request_input(PinConfig{gpio_chip, hook_detect_line, "icom2000-tcm1171-hook"},
                                         Edge::Both),
        },
        loop);

    StatusLed status_led(gpio_backend->request_output(
        PinConfig{gpio_chip, status_led_line, "icom2000-status-led"}, Level::Low));

    auto audio = icom::audio::make_null_audio_engine(stations);
    audio->start();

    icom::ipc::ControlServer server(socket_path, loop);

    server.register_command("PING", [](const auto&) { return CommandResult::success("pong"); });

    server.register_command("BELL", [&](const std::vector<std::string>& args) {
        if (args.empty()) {
            return CommandResult::failure("usage: BELL ON|OFF|RING [MS]");
        }
        const std::string sub = to_upper(args[0]);
        if (sub == "ON") {
            bell.ring();
        } else if (sub == "OFF") {
            bell.silence();
        } else if (sub == "RING") {
            const int ms = args.size() > 1 ? std::atoi(args[1].c_str()) : 1000;
            bell.ring_for(std::chrono::milliseconds(ms), loop);
        } else {
            return CommandResult::failure("usage: BELL ON|OFF|RING [MS]");
        }
        return CommandResult::success(bell.is_ringing() ? "ringing" : "silent");
    });

    server.register_command("LINE", [&](const std::vector<std::string>& args) {
        if (args.empty() || to_upper(args[0]) != "STATUS") {
            return CommandResult::failure("usage: LINE STATUS");
        }
        return CommandResult::success(icom::hw::to_string(line.state()));
    });

    server.register_command("STATUS", [&](const auto&) {
        return CommandResult::success(std::string("line=") + icom::hw::to_string(line.state()) +
                                       " bell=" + (bell.is_ringing() ? "ringing" : "silent") +
                                       " audio=" + (audio->is_running() ? "up" : "down"));
    });

    server.start();

    // Visual "the daemon is up and its main loop is about to start"
    // signal -- fire-and-forget, scheduled on `loop` itself rather than
    // blocking startup for the ~900ms the full sequence takes.
    status_led.blink_n_times(3, loop);

    kLog.info("ready");
    loop.run();

    kLog.info("stopped");
    audio->stop();
    return 0;
}
