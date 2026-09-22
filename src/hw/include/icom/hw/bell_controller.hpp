#pragma once

#include "icom/core/event_loop.hpp"
#include "icom/hw/pwm.hpp"

#include <chrono>
#include <memory>

namespace icom::hw {

// A door bell relay/buzzer, driven via software PWM (icom::hw::Pwm)
// rather than a plain on/off GPIO write -- ring()/silence() just move the
// PWM's duty time between `ringDutyTime` (constructor parameter) and
// zero, so callers see the same simple two-state API as a plain digital
// pin (this used to BE just one IOutputPin; see Tcm1171Controller for how
// a stateful, event-driven component built directly on IOutputPin/IInputPin
// would look). PWM matters here because the bell is a relay/buzzer, not a
// clean digital load: driving it at less than 100% duty controls how
// hard it strikes/how loud it buzzes, and continuous full-power drive on
// some relay coils is exactly the kind of thing that overheats them.
class BellController {
public:
    // `pwm` should already be constructed on a icom::core::LoopThread's
    // loop (see that class), not the daemon's main one -- see
    // docs/ARCHITECTURE.md "Software PWM" for why. `ringDutyTime`,
    // clamped to `pwm`'s period, is what ring() drives the duty to;
    // defaulting it to the full period reproduces the old plain-digital
    // "just turn it on" behavior for anyone who hasn't tuned it yet.
    explicit BellController(std::unique_ptr<Pwm> pwm,
                            std::chrono::milliseconds ringDutyTime = std::chrono::milliseconds::max());

    void ring();
    void silence();
    bool isRinging() const;

    // Convenience for "ring for exactly this long": schedules silence() on
    // the given EventLoop. The loop must outlive the returned call, i.e.
    // don't call this after the loop has stopped for good. This is a
    // SEPARATE loop/timer from the PWM's own cycling -- just "when to stop
    // ringing", low-frequency enough that the daemon's main loop is the
    // right place for it (see Pwm's own header for why the PWM cycle
    // itself needs a dedicated loop and this doesn't).
    void ringFor(std::chrono::milliseconds duration, core::EventLoop& loop);

private:
    std::unique_ptr<Pwm> pwm_;
    std::chrono::milliseconds ringDutyTime_;
    bool ringing_ = false;
};

} // namespace icom::hw
