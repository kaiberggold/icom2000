// intercomd: the composition root. Every concrete type gets constructed
// here and wired together through the interfaces defined in each module;
// nothing above this file knows about libgpiod, ALSA, or Unix sockets, and
// nothing below it knows about the others. See docs/ARCHITECTURE.md.
#include "icom/core/event_loop.hpp"
#include "icom/core/logger.hpp"
#include "icom/core/signal_watcher.hpp"
#include "icom/gpio/digital_pin.hpp"
#include "icom/hw/bell_controller.hpp"
#include "icom/hw/tcm1171_controller.hpp"
#include "icom/audio/audio_engine.hpp"
#include "icom/ipc/control_server.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using icom::core::EventLoop;
using icom::core::LogLevel;
using icom::core::SignalWatcher;
using icom::gpio::Edge;
using icom::gpio::Level;
using icom::gpio::PinConfig;
using icom::hw::BellController;
using icom::hw::LineState;
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
    std::string socket_path = "/run/icom2000.sock";
    std::string gpio_chip = "gpiochip0";
};

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            opts.socket_path = argv[++i];
        } else if (arg == "--gpio-chip" && i + 1 < argc) {
            opts.gpio_chip = argv[++i];
        } else if (arg == "--help") {
            std::cout << "usage: intercomd [--socket PATH] [--gpio-chip NAME]\n";
            std::exit(0);
        }
    }
    return opts;
}

// BCM/line numbers below are placeholders for a sketch, not a verified
// wiring diagram -- see docs/ARCHITECTURE.md "Pin assignments" before
// touching real hardware.
constexpr unsigned kBellPinLine = 17;
constexpr unsigned kRingModePinLine = 27;
constexpr unsigned kPolarityPinLine = 22;
constexpr unsigned kHookDetectPinLine = 23;

} // namespace

int main(int argc, char** argv) {
    icom::core::set_min_log_level(LogLevel::Debug);
    const Options opts = parse_args(argc, argv);

    icom::core::log_info("intercomd starting (socket=" + opts.socket_path +
                          ", gpio-chip=" + opts.gpio_chip + ")");

    EventLoop loop;

    SignalWatcher signals(loop, {SIGINT, SIGTERM}, [&](int signo) {
        icom::core::log_info("received signal " + std::to_string(signo) + ", shutting down");
        loop.stop();
    });

    auto gpio_backend = icom::gpio::make_default_backend();

    BellController bell(gpio_backend->request_output(
        PinConfig{opts.gpio_chip, kBellPinLine, "icom2000-bell"}, Level::Low));

    Tcm1171Controller line(
        Tcm1171Controller::Pins{
            gpio_backend->request_output(PinConfig{opts.gpio_chip, kRingModePinLine, "icom2000-tcm1171-rm"},
                                          Level::Low),
            gpio_backend->request_output(PinConfig{opts.gpio_chip, kPolarityPinLine, "icom2000-tcm1171-fr"},
                                          Level::Low),
            gpio_backend->request_input(PinConfig{opts.gpio_chip, kHookDetectPinLine, "icom2000-tcm1171-hook"},
                                         Edge::Both),
        },
        loop);

    auto audio = icom::audio::make_null_audio_engine();
    audio->start();

    icom::ipc::ControlServer server(opts.socket_path, loop);

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

    icom::core::log_info("intercomd ready");
    loop.run();

    icom::core::log_info("intercomd stopped");
    audio->stop();
    return 0;
}
