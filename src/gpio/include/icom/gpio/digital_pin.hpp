#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace icom::gpio {

enum class Level : bool { LOW = false, HIGH = true };

enum class Edge { RISING, FALLING, BOTH };

// Which physical GPIO line a pin binds to. `chip` is a libgpiod chip label
// (e.g. "gpiochip0"); `line` is that chip's line offset, not a BCM number,
// though on the Pi Zero's SoC gpiochip0 offsets happen to match BCM numbers.
// `consumer` is the label that shows up in `gpioinfo` -- set it to something
// that identifies the caller (e.g. "icom2000-bell") so a live system is
// debuggable from the shell.
struct PinConfig {
    std::string chip;
    unsigned line = 0;
    std::string consumer;
};

// A GPIO line driven by this process.
class OutputPin {
public:
    virtual ~OutputPin() = default;

    virtual void write(Level level) = 0;

    // Last level this process drove -- not a read of the physical pin, since
    // most backends (including libgpiod on an output line) don't expose that.
    virtual Level drivenLevel() const = 0;
};

// A GPIO line this process only reads, with optional edge notification.
class InputPin {
public:
    // `at` is when the kernel observed the edge (CLOCK_MONOTONIC), not when
    // this callback happens to run -- useful for pulse-dial timing where the
    // delay through the reactor should not be counted.
    using EdgeCallback = std::function<void(Level level, std::chrono::steady_clock::time_point at)>;

    virtual ~InputPin() = default;

    virtual Level read() const = 0;

    // A pollable descriptor that becomes readable when an edge event is
    // pending, or -1 if this backend has no edge support (caller must poll
    // read() on a timer instead). Owned by the pin; do not close it.
    virtual int eventFd() const = 0;

    // Drains pending edge events from eventFd() and invokes `callback` for
    // each one, in order. Call this from the EventLoop callback registered
    // against eventFd().
    virtual void consumeEvents(const EdgeCallback& callback) = 0;
};

// Factory boundary between the hardware-agnostic control logic and whichever
// backend was compiled in (see src/gpio/src/mock and src/gpio/src/gpiod).
// Keeping this as an interface -- rather than having BellController etc.
// call libgpiod directly -- is what lets the whole daemon build and run its
// unit tests on a dev host with no GPIO chip at all.
class Backend {
public:
    virtual ~Backend() = default;

    virtual std::unique_ptr<OutputPin> requestOutput(const PinConfig& config, Level initial) = 0;
    virtual std::unique_ptr<InputPin> requestInput(const PinConfig& config, Edge edge) = 0;
};

// Returns the backend selected at compile time (ICOM_WITH_LIBGPIOD): the
// real libgpiod backend on-target, or an in-memory mock for host dev/tests.
std::unique_ptr<Backend> makeDefaultBackend();

} // namespace icom::gpio
