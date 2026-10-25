// Covers icom::hw::Tcm1171Controller against the mock GPIO backend: the
// optional line mute (absent by default, see config/icom2000.conf's
// [gpio.tcm1171] mute_line), active high or low (mute_active), and the
// hook-edge state changes it has to
// stay independent of. Edges go through a real EventLoop, the same path
// the controller takes in intercomd.
#include "check.hpp"

#include "icom/core/event_loop.hpp"
#include "icom/gpio/mock_backend.hpp"
#include "icom/hw/tcm1171_controller.hpp"

#include <chrono>

using namespace icom;

namespace
{

    struct Line
    {
        gpio::MockBackend backend;
        core::EventLoop loop;
        gpio::IOutputPin* ringMode = nullptr;
        gpio::IOutputPin* mute = nullptr;
        gpio::IInputPin* hookDetect = nullptr;
        std::unique_ptr<hw::Tcm1171Controller> controller;

        // `muteInitial` is what the backend starts the mute pin at, to show
        // the controller itself drives it to unmuted; `muteActive` the
        // level that means muted.
        explicit Line(bool withMute, gpio::Level muteInitial = gpio::Level::LOW,
                      gpio::Level muteActive = gpio::Level::HIGH)
        {
            hw::Tcm1171Controller::Pins pins
            {
                backend.requestOutput(gpio::PinConfig{"mockchip0", 5, "test-rm"}, gpio::Level::LOW),
                backend.requestOutput(gpio::PinConfig{"mockchip0", 22, "test-fr"}, gpio::Level::LOW),
                backend.requestInput(gpio::PinConfig{"mockchip0", 6, "test-hook"}, gpio::Edge::BOTH),
                withMute ? backend.requestOutput(gpio::PinConfig{"mockchip0", 26, "test-mute"}, muteInitial)
                    : nullptr,
                      muteActive,
                };
            ringMode = pins.ringMode.get();
            mute = pins.mute.get();
            hookDetect = pins.hookDetect.get();
            controller = std::make_unique<hw::Tcm1171Controller>(std::move(pins), loop);
        }

        // Delivers one simulated hook edge through the loop.
        void edge(gpio::Level level)
        {
            CHECK(gpio::injectMockEdge(*hookDetect, level));
            loop.addTimer(std::chrono::milliseconds(20), /*repeat=*/false, [this] { loop.stop(); });
            loop.run();
        }
    };

    void testWithoutMuteLineMuteIsUnavailable()
    {
        Line line(/*withMute=*/false);

        CHECK(!line.controller->hasMute());
        CHECK(!line.controller->isMuted());
        CHECK(!line.controller->setMuted(true));
        CHECK(!line.controller->isMuted());
        CHECK(line.controller->setMuted(false) == false);
    }

    void testMuteStartsUnmutedAndDrivesThePin()
    {
        Line line(/*withMute=*/true, gpio::Level::HIGH);

        CHECK(line.controller->hasMute());
        CHECK(!line.controller->isMuted());
        CHECK(line.mute->drivenLevel() == gpio::Level::LOW);

        CHECK(line.controller->setMuted(true));
        CHECK(line.controller->isMuted());
        CHECK(line.mute->drivenLevel() == gpio::Level::HIGH);

        CHECK(line.controller->setMuted(true)); // already muted: still fine
        CHECK(line.mute->drivenLevel() == gpio::Level::HIGH);

        CHECK(line.controller->setMuted(false));
        CHECK(!line.controller->isMuted());
        CHECK(line.mute->drivenLevel() == gpio::Level::LOW);
    }

    void testActiveLowMuteDrivesThePinInverted()
    {
        Line line(/*withMute=*/true, gpio::Level::LOW, /*muteActive=*/gpio::Level::LOW);

        CHECK(!line.controller->isMuted());
        CHECK(line.mute->drivenLevel() == gpio::Level::HIGH);

        CHECK(line.controller->setMuted(true));
        CHECK(line.controller->isMuted());
        CHECK(line.mute->drivenLevel() == gpio::Level::LOW);

        CHECK(line.controller->setMuted(false));
        CHECK(!line.controller->isMuted());
        CHECK(line.mute->drivenLevel() == gpio::Level::HIGH);
    }

    void testMuteIsIndependentOfLineState()
    {
        Line line(/*withMute=*/true);
        CHECK(line.controller->setMuted(true));

        line.edge(gpio::Level::HIGH);
        CHECK(line.controller->state() == hw::LineState::OFF_HOOK);
        CHECK(line.controller->isMuted());

        line.edge(gpio::Level::LOW);
        CHECK(line.controller->state() == hw::LineState::ON_HOOK);
        CHECK(line.controller->isMuted());

        line.controller->startRinging();
        CHECK(line.controller->state() == hw::LineState::RINGING);
        CHECK(line.controller->isMuted());

        line.controller->stopRinging();
        CHECK(line.controller->setMuted(false));
        line.edge(gpio::Level::HIGH);
        CHECK(!line.controller->isMuted());
    }

    void testAnsweringWhileRingingStopsRinging()
    {
        Line line(/*withMute=*/false);

        line.controller->startRinging();
        CHECK(line.controller->state() == hw::LineState::RINGING);
        CHECK(line.ringMode->drivenLevel() == gpio::Level::HIGH);

        line.edge(gpio::Level::HIGH);
        CHECK(line.controller->state() == hw::LineState::OFF_HOOK);
        CHECK(line.ringMode->drivenLevel() == gpio::Level::LOW);
    }

} // namespace

int main()
{
    testWithoutMuteLineMuteIsUnavailable();
    testMuteStartsUnmutedAndDrivesThePin();
    testActiveLowMuteDrivesThePinInverted();
    testMuteIsIndependentOfLineState();
    testAnsweringWhileRingingStopsRinging();

    const int failures = icom::testing::failureCount();
    if (failures > 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
