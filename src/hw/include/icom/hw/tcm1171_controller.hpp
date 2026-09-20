#pragma once

#include "icom/core/event_loop.hpp"
#include "icom/gpio/digital_pin.hpp"

#include <atomic>
#include <functional>
#include <memory>

namespace icom::hw {

// Line state as seen from the TCM1171 SLIC side of an old analog handset.
enum class LineState {
    ON_HOOK,   // handset resting -- idle
    RINGING,   // we are driving ring voltage onto the line
    OFF_HOOK,  // handset lifted -- a call in progress
    FAULT,     // unexpected/contradictory GPIO state; needs investigation
};

const char* toString(LineState state);

// Drives and monitors an old analog telephone through a TCM1171-family SLIC
// (Subscriber Line Interface Circuit).
//
// *** Pin mapping is provisional. *** The TCM1171 itself exposes two digital
// control inputs relevant here -- FR (forward/reverse line polarity) and RM
// (ring mode enable) -- but "is the handset off-hook" is normally derived
// from the chip's analog loop-current-sense pin (IL) through an external
// comparator, not a direct digital output on the SLIC. `Pins::hookDetect`
// below stands in for "whatever GPIO that external comparator/optocoupler
// drives" until the real schematic is finalized; treat the exact pin count
// and polarity here as a sketch to be corrected against the datasheet and
// board wiring, not as verified hardware fact.
class Tcm1171Controller {
public:
    struct Pins {
        std::unique_ptr<gpio::OutputPin> ringMode;    // RM: enable ring generation on the line
        std::unique_ptr<gpio::OutputPin> polarity;     // FR: forward/reverse line polarity
        std::unique_ptr<gpio::InputPin> hookDetect;   // external loop-current comparator output
    };

    using StateChangeCallback = std::function<void(LineState previous, LineState current)>;

    // Registers hookDetect's edge fd with `loop` so state transitions
    // happen as edges arrive; `loop` must outlive this controller.
    Tcm1171Controller(Pins pins, core::EventLoop& loop, StateChangeCallback onChange = {});

    LineState state() const;

    // Starts/stops driving ring voltage. No-op (logged, not asserted) if
    // called from a state where it doesn't make sense, e.g. startRinging()
    // while already OffHook -- callers are expected to check state() first,
    // but this must never be allowed to wedge the line.
    void startRinging();
    void stopRinging();

private:
    void setState(LineState next);
    void onHookEdge(gpio::Level level, std::chrono::steady_clock::time_point at);

    Pins pins_;
    core::EventLoop& loop_;
    StateChangeCallback onChange_;
    std::atomic<LineState> state_{LineState::ON_HOOK};
};

} // namespace icom::hw
