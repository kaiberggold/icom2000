#pragma once

#include "icom/core/event_loop.hpp"
#include "icom/gpio/digital_pin.hpp"

#include <atomic>
#include <functional>
#include <memory>

namespace icom::hw {

// Line state as seen from the TCM1171 SLIC side of an old analog handset.
enum class LineState {
    OnHook,   // handset resting -- idle
    Ringing,  // we are driving ring voltage onto the line
    OffHook,  // handset lifted -- a call in progress
    Fault,    // unexpected/contradictory GPIO state; needs investigation
};

const char* to_string(LineState state);

// Drives and monitors an old analog telephone through a TCM1171-family SLIC
// (Subscriber Line Interface Circuit).
//
// *** Pin mapping is provisional. *** The TCM1171 itself exposes two digital
// control inputs relevant here -- FR (forward/reverse line polarity) and RM
// (ring mode enable) -- but "is the handset off-hook" is normally derived
// from the chip's analog loop-current-sense pin (IL) through an external
// comparator, not a direct digital output on the SLIC. `Pins::hook_detect`
// below stands in for "whatever GPIO that external comparator/optocoupler
// drives" until the real schematic is finalized; treat the exact pin count
// and polarity here as a sketch to be corrected against the datasheet and
// board wiring, not as verified hardware fact.
class Tcm1171Controller {
public:
    struct Pins {
        std::unique_ptr<gpio::OutputPin> ring_mode;    // RM: enable ring generation on the line
        std::unique_ptr<gpio::OutputPin> polarity;     // FR: forward/reverse line polarity
        std::unique_ptr<gpio::InputPin> hook_detect;   // external loop-current comparator output
    };

    using StateChangeCallback = std::function<void(LineState previous, LineState current)>;

    // Registers hook_detect's edge fd with `loop` so state transitions
    // happen as edges arrive; `loop` must outlive this controller.
    Tcm1171Controller(Pins pins, core::EventLoop& loop, StateChangeCallback on_change = {});

    LineState state() const;

    // Starts/stops driving ring voltage. No-op (logged, not asserted) if
    // called from a state where it doesn't make sense, e.g. start_ringing()
    // while already OffHook -- callers are expected to check state() first,
    // but this must never be allowed to wedge the line.
    void start_ringing();
    void stop_ringing();

private:
    void set_state(LineState next);
    void on_hook_edge(gpio::Level level, std::chrono::steady_clock::time_point at);

    Pins pins_;
    core::EventLoop& loop_;
    StateChangeCallback on_change_;
    std::atomic<LineState> state_{LineState::OnHook};
};

} // namespace icom::hw
