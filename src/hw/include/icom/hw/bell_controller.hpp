#pragma once

#include "icom/core/event_loop.hpp"
#include "icom/gpio/digital_pin.hpp"

#include <memory>

namespace icom::hw {

// The concrete "one working digital pin" example: a door bell relay/buzzer
// driven by a single GPIO output. Deliberately the simplest possible
// component so it's easy to read end-to-end -- see Tcm1171Controller for
// how a stateful, event-driven component built on the same OutputPin/
// InputPin interfaces would look.
class BellController {
public:
    // `pin` is expected to be wired active-high into a relay/MOSFET driving
    // the bell -- flip the ring()/silence() bodies if your board is
    // active-low.
    explicit BellController(std::unique_ptr<gpio::OutputPin> pin);

    void ring();
    void silence();
    bool isRinging() const;

    // Convenience for "ring for exactly this long": schedules silence() on
    // the given EventLoop. The loop must outlive the returned call, i.e.
    // don't call this after the loop has stopped for good.
    void ringFor(std::chrono::milliseconds duration, core::EventLoop& loop);

private:
    std::unique_ptr<gpio::OutputPin> pin_;
    bool ringing_ = false;
};

} // namespace icom::hw
