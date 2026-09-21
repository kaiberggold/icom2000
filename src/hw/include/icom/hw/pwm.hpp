#pragma once

#include "icom/core/event_loop.hpp"
#include "icom/gpio/digital_pin.hpp"

#include <atomic>
#include <chrono>
#include <memory>

namespace icom::hw {

// Software PWM: cycles one GPIO output High for `dutyTime` out of every
// `period`, indefinitely, until destroyed. Meant to run on a
// icom::core::LoopThread's own loop rather than the daemon's main one --
// see that class's header comment -- so the cycle's timing never has to
// wait its turn behind an IPC command or a GPIO edge on the main
// EventLoop's poll().
//
// Every public method here is safe to call from any thread (not just
// `loop`'s own), including the constructor and destructor: internally,
// anything that touches `loop` funnels through EventLoop::post() so it
// actually runs on `loop`'s own thread, and the destructor blocks until
// its own posted teardown has actually completed there before returning
// -- required so a cycle in flight can never fire into a destroyed Pwm.
//
// That blocking wait needs someone to still be calling `loop.run()` --
// actively dispatching, not merely constructed -- for as long as this
// object exists, or it hangs forever with nothing left to service the
// post(). A icom::core::LoopThread's own loop satisfies this by
// construction (its background thread calls run() for the LoopThread's
// entire lifetime); the daemon's main loop does NOT once its own run()
// has returned during shutdown, which is exactly why this is meant for a
// LoopThread and not the main loop.
class Pwm {
public:
    // `pin` is driven High for `dutyTime` (initially zero -- silent until
    // setDutyTime() says otherwise) out of every `period`. `loop` must
    // outlive this object.
    Pwm(std::unique_ptr<gpio::OutputPin> pin, core::EventLoop& loop,
        std::chrono::milliseconds period = std::chrono::milliseconds(10));
    ~Pwm();

    Pwm(const Pwm&) = delete;
    Pwm& operator=(const Pwm&) = delete;

    // Changes the on-time within each period, clamped to [0, period()].
    // Takes effect at the start of the next period, never mid-cycle, so a
    // change in flight can't produce a truncated or stretched pulse.
    void setDutyTime(std::chrono::milliseconds dutyTime);
    std::chrono::milliseconds dutyTime() const { return dutyTime_.load(); }

    std::chrono::milliseconds period() const { return period_; }

private:
    // Called at the start of each period (always on loop_'s own thread):
    // decides whether this period is fully off, fully on, or needs a
    // mid-period transition, and schedules the next call accordingly.
    void startPeriod();
    // Mid-period transition for the "on for part of the period" case --
    // `dutyThisPeriod` is the value startPeriod() read when it turned the
    // pin on, deliberately NOT re-read here: a setDutyTime() racing in
    // mid-period must not shorten/stretch the period already in flight.
    void turnOffMidPeriod(std::chrono::milliseconds dutyThisPeriod);

    std::unique_ptr<gpio::OutputPin> pin_;
    core::EventLoop& loop_;
    std::chrono::milliseconds period_;
    std::atomic<std::chrono::milliseconds> dutyTime_;
    core::EventLoop::TimerId pendingTimerId_ = 0; // loop_ thread only
};

} // namespace icom::hw
