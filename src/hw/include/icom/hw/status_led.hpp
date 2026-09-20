#pragma once

#include "icom/core/event_loop.hpp"
#include "icom/gpio/digital_pin.hpp"

#include <chrono>
#include <memory>

namespace icom::hw {

// Drives a single GPIO-connected status LED. As simple as BellController
// (see that class for the "one output pin" baseline this mirrors) plus one
// addition: blink_n_times(), a fire-and-forget startup/self-test indicator
// scheduled on the EventLoop rather than blocking the caller -- safe to
// call before loop.run() so the blinking happens as the daemon's main loop
// starts up, without delaying anything else in main() while it runs.
class StatusLed {
public:
    // `pin` is expected to be wired active-high (LED lights when driven
    // High) -- flip on()/off() if your board wires it active-low.
    explicit StatusLed(std::unique_ptr<gpio::OutputPin> pin);

    void on();
    void off();
    bool is_on() const;

    // Schedules `times` on/off blinks on `loop` and returns immediately;
    // `loop` must outlive the sequence (in particular, must not be
    // destroyed before it's run at least `times` on/off cycles' worth).
    // A no-op if `times` is 0.
    void blink_n_times(unsigned times, core::EventLoop& loop,
                        std::chrono::milliseconds on_duration = std::chrono::milliseconds(150),
                        std::chrono::milliseconds off_duration = std::chrono::milliseconds(150));

private:
    std::unique_ptr<gpio::OutputPin> pin_;
    bool on_ = false;
};

} // namespace icom::hw
