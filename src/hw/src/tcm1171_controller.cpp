#include "icom/hw/tcm1171_controller.hpp"
#include "icom/core/logger.hpp"

#include <poll.h>

namespace icom::hw
{

    namespace
    {
        core::Logger& log = core::getLogger("hw.tcm1171");
    } // namespace

    const char* toString(LineState state)
    {
        switch (state)
        {
            case LineState::ON_HOOK:  return "on-hook";
            case LineState::RINGING: return "ringing";
            case LineState::OFF_HOOK: return "off-hook";
            case LineState::FAULT:   return "fault";
        }
        return "unknown";
    }

    Tcm1171Controller::Tcm1171Controller(Pins pins, core::EventLoop& loop, StateChangeCallback onChange)
        : pins_(std::move(pins)), loop_(loop), onChange_(std::move(onChange))
    {
        pins_.ringMode->write(gpio::Level::LOW);
        pins_.polarity->write(gpio::Level::LOW);

        const int fd = pins_.hookDetect->eventFd();
        if (fd >= 0)
        {
            loop_.addFd(fd, POLLIN, [this](short)
            {
                pins_.hookDetect->consumeEvents(
                    [this](gpio::Level level, std::chrono::steady_clock::time_point at)
                {
                    onHookEdge(level, at);
                });
            });
        }
        else
        {
            log.warn("hook_detect pin has no edge support; hook state will never update");
        }
    }

    LineState Tcm1171Controller::state() const { return state_.load(); }

    void Tcm1171Controller::startRinging()
    {
        if (state() != LineState::ON_HOOK)
        {
            log.warn(std::string("refusing to start ringing from state ") + toString(state()));
            return;
        }
        pins_.ringMode->write(gpio::Level::HIGH);
        setState(LineState::RINGING);
    }

    void Tcm1171Controller::stopRinging()
    {
        if (state() != LineState::RINGING)
        {
            return;
        }
        pins_.ringMode->write(gpio::Level::LOW);
        setState(LineState::ON_HOOK);
    }

    void Tcm1171Controller::setState(LineState next)
    {
        const LineState previous = state_.exchange(next);
        if (previous == next)
        {
            return;
        }
        log.info(std::string(toString(previous)) + " -> " + toString(next));
        if (onChange_)
        {
            onChange_(previous, next);
        }
    }

    void Tcm1171Controller::onHookEdge(gpio::Level level, std::chrono::steady_clock::time_point /*at*/)
    {
        // Placeholder polarity: High == handset lifted (off-hook). Flip this
        // once the comparator's actual sense is confirmed against hardware.
        //
        // Pulse dialing would also show up here as a train of brief low edges
        // while off-hook -- not decoded yet; that logic belongs in a
        // PulseDialDecoder consuming these same edges, timestamped by `at`
        // rather than by when this callback happens to run on the reactor.
        if (level == gpio::Level::HIGH)
        {
            if (state() == LineState::RINGING)
            {
                // Callee picked up while we were ringing -- stop driving ring
                // voltage before treating the line as off-hook.
                pins_.ringMode->write(gpio::Level::LOW);
            }
            setState(LineState::OFF_HOOK);
        }
        else
        {
            setState(LineState::ON_HOOK);
        }
    }

} // namespace icom::hw
