#include "icom/hw/status_led.hpp"
#include "icom/core/logger.hpp"

namespace icom::hw {

namespace {
core::Logger& kLog = core::get_logger("hw.status_led");

// Free function, not a member lambda that captures itself: a self-owning
// std::function (one that captures a shared_ptr to the state it also
// lives inside) is a reference cycle that never gets freed. Each
// recursive step here instead schedules a *new* lambda that only captures
// a copy of `state` -- state itself never holds a callback, so nothing
// keeps itself alive.
struct BlinkState {
    StatusLed* led;
    unsigned remaining; // on/off cycles left to complete
    std::chrono::milliseconds on_duration;
    std::chrono::milliseconds off_duration;
};

void blink_step(std::shared_ptr<BlinkState> state, core::EventLoop& loop) {
    if (state->led->is_on()) {
        state->led->off();
        if (--state->remaining == 0) {
            return; // sequence complete
        }
        loop.add_timer(state->off_duration, /*repeat=*/false,
                        [state, &loop] { blink_step(state, loop); });
    } else {
        state->led->on();
        loop.add_timer(state->on_duration, /*repeat=*/false,
                        [state, &loop] { blink_step(state, loop); });
    }
}

} // namespace

StatusLed::StatusLed(std::unique_ptr<gpio::OutputPin> pin) : pin_(std::move(pin)) {
    pin_->write(gpio::Level::Low);
}

void StatusLed::on() {
    pin_->write(gpio::Level::High);
    on_ = true;
}

void StatusLed::off() {
    pin_->write(gpio::Level::Low);
    on_ = false;
}

bool StatusLed::is_on() const { return on_; }

void StatusLed::blink_n_times(unsigned times, core::EventLoop& loop,
                               std::chrono::milliseconds on_duration,
                               std::chrono::milliseconds off_duration) {
    if (times == 0) {
        return;
    }
    kLog.debug("blinking " + std::to_string(times) + " time(s)");
    on();
    auto state = std::make_shared<BlinkState>(BlinkState{this, times, on_duration, off_duration});
    loop.add_timer(on_duration, /*repeat=*/false, [state, &loop] { blink_step(state, loop); });
}

} // namespace icom::hw
