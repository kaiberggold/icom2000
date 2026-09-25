#pragma once

#include "icom/core/event_loop.hpp"
#include "icom/gpio/digital_pin.hpp"

#include <atomic>
#include <chrono>
#include <memory>

namespace icom::hw {

// Drives a single GPIO-connected status LED. As simple as BellController
// (see that class for the "one output pin" baseline this mirrors) plus one
// addition: blinkNTimes(), a fire-and-forget startup/self-test indicator
// scheduled on an EventLoop rather than blocking the caller.
//
// `loop` is typically a icom::core::LoopThread's own loop (post()'d there,
// since only post() is safe to call cross-thread) rather than the
// daemon's main one -- e.g. to share a loop with icom::hw::Pwm's cycling,
// which is what daemon_main.cpp actually does. Because a blink sequence
// can genuinely be in flight on a thread other than whichever one
// destroys this object, the destructor blocks until it's confirmed (via
// that same loop, synchronously) that no further blink step will touch
// `this` -- the same reasoning, and the same pattern, as Pwm's destructor;
// see that class's header comment for the fuller explanation of why a
// blocking wait is what's actually required here (not just a "stop"
// flag), and for why that wait needs `loop` to still be actively running
// (inside run(), on some thread) at destruction time -- true for a
// LoopThread's own loop for as long as the LoopThread exists, NOT true
// for the daemon's main loop once its own run() has returned. Only
// destroy this object while blinkNTimes()'s last `loop` argument is still
// being serviced.
class StatusLed {
public:
    // `pin` is expected to be wired active-high (LED lights when driven
    // High) -- flip on()/off() if your board wires it active-low.
    explicit StatusLed(std::unique_ptr<gpio::IOutputPin> pin);
    ~StatusLed();

    StatusLed(const StatusLed&) = delete;
    StatusLed& operator=(const StatusLed&) = delete;

    void on();
    void off();
    bool isOn() const { return on_.load(); }

    // Schedules `times` on/off blinks on `loop` and returns immediately;
    // `loop` must outlive this object. A no-op if `times` is 0. NOT safe
    // to call from a thread other than `loop`'s own -- if `loop` is a
    // LoopThread's, call this from inside a lambda given to that loop's
    // post(), not directly. Calling this again before a previous sequence
    // has finished replaces it rather than running both -- there is only
    // one in-flight sequence at a time.
    void blinkNTimes(unsigned times, core::EventLoop& loop,
                     std::chrono::milliseconds onDuration = std::chrono::milliseconds(150),
                     std::chrono::milliseconds offDuration = std::chrono::milliseconds(150));

private:
    void blinkStep(); // runs only on blinkLoop_'s own thread

    std::unique_ptr<gpio::IOutputPin> pin_;
    std::atomic<bool> on_ = false;

    // Set by blinkNTimes() (on the loop's own thread, per that method's
    // contract), read by ~StatusLed() (possibly a different thread) to
    // know which loop to post the cancellation to -- hence atomic, unlike
    // the rest of this state, which blinkStep() only ever touches from
    // blinkLoop_'s own thread and so needs no synchronization.
    std::atomic<core::EventLoop*> blinkLoop_ = nullptr;
    unsigned remaining_ = 0;
    std::chrono::milliseconds onDuration_{};
    std::chrono::milliseconds offDuration_{};
    bool cancelled_ = false;
};

} // namespace icom::hw
