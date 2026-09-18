#include "icom/hw/tcm1171_controller.hpp"
#include "icom/core/logger.hpp"

#include <poll.h>

namespace icom::hw {

namespace {
core::Logger& kLog = core::get_logger("hw.tcm1171");
} // namespace

const char* to_string(LineState state) {
    switch (state) {
        case LineState::OnHook:  return "on-hook";
        case LineState::Ringing: return "ringing";
        case LineState::OffHook: return "off-hook";
        case LineState::Fault:   return "fault";
    }
    return "unknown";
}

Tcm1171Controller::Tcm1171Controller(Pins pins, core::EventLoop& loop, StateChangeCallback on_change)
    : pins_(std::move(pins)), loop_(loop), on_change_(std::move(on_change)) {
    pins_.ring_mode->write(gpio::Level::Low);
    pins_.polarity->write(gpio::Level::Low);

    const int fd = pins_.hook_detect->event_fd();
    if (fd >= 0) {
        loop_.add_fd(fd, POLLIN, [this](short) {
            pins_.hook_detect->consume_events(
                [this](gpio::Level level, std::chrono::steady_clock::time_point at) {
                    on_hook_edge(level, at);
                });
        });
    } else {
        kLog.warn("hook_detect pin has no edge support; hook state will never update");
    }
}

LineState Tcm1171Controller::state() const { return state_.load(); }

void Tcm1171Controller::start_ringing() {
    if (state() != LineState::OnHook) {
        kLog.warn(std::string("refusing to start ringing from state ") + to_string(state()));
        return;
    }
    pins_.ring_mode->write(gpio::Level::High);
    set_state(LineState::Ringing);
}

void Tcm1171Controller::stop_ringing() {
    if (state() != LineState::Ringing) {
        return;
    }
    pins_.ring_mode->write(gpio::Level::Low);
    set_state(LineState::OnHook);
}

void Tcm1171Controller::set_state(LineState next) {
    const LineState previous = state_.exchange(next);
    if (previous == next) {
        return;
    }
    kLog.info(std::string(to_string(previous)) + " -> " + to_string(next));
    if (on_change_) {
        on_change_(previous, next);
    }
}

void Tcm1171Controller::on_hook_edge(gpio::Level level, std::chrono::steady_clock::time_point /*at*/) {
    // Placeholder polarity: High == handset lifted (off-hook). Flip this
    // once the comparator's actual sense is confirmed against hardware.
    //
    // Pulse dialing would also show up here as a train of brief low edges
    // while off-hook -- not decoded yet; that logic belongs in a
    // PulseDialDecoder consuming these same edges, timestamped by `at`
    // rather than by when this callback happens to run on the reactor.
    if (level == gpio::Level::High) {
        if (state() == LineState::Ringing) {
            // Callee picked up while we were ringing -- stop driving ring
            // voltage before treating the line as off-hook.
            pins_.ring_mode->write(gpio::Level::Low);
        }
        set_state(LineState::OffHook);
    } else {
        set_state(LineState::OnHook);
    }
}

} // namespace icom::hw
