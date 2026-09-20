// Exercises the two pieces a dev host can actually test without real
// hardware: the mock GPIO backend, and the EventLoop reactor it's designed
// to plug into. Together they stand in for what would otherwise need a
// physical GPIO chip -- this is the payoff of the OutputPin/InputPin
// interface split in icom/gpio/digital_pin.hpp.
#include "check.hpp"

#include "icom/core/event_loop.hpp"
#include "icom/gpio/mock_backend.hpp"

#include <chrono>

#include <poll.h>

using namespace icom;

namespace {

void testOutputPinTracksDrivenLevel() {
    gpio::MockBackend backend;
    auto pin = backend.requestOutput(gpio::PinConfig{"mockchip0", 1, "test-output"}, gpio::Level::LOW);

    CHECK(pin->drivenLevel() == gpio::Level::LOW);
    pin->write(gpio::Level::HIGH);
    CHECK(pin->drivenLevel() == gpio::Level::HIGH);
}

void testInputPinEdgeReachesEventLoop() {
    gpio::MockBackend backend;
    auto pin = backend.requestInput(gpio::PinConfig{"mockchip0", 2, "test-input"}, gpio::Edge::BOTH);

    core::EventLoop loop;
    bool gotEdge = false;
    gpio::Level gotLevel = gpio::Level::LOW;

    loop.addFd(pin->eventFd(), POLLIN, [&](short) {
        pin->consumeEvents([&](gpio::Level level, std::chrono::steady_clock::time_point) {
            gotEdge = true;
            gotLevel = level;
        });
        loop.stop();
    });

    // Safety net: if the edge never reaches the loop, don't hang the suite.
    loop.addTimer(std::chrono::milliseconds(500), /*repeat=*/false, [&] { loop.stop(); });

    CHECK(gpio::injectMockEdge(*pin, gpio::Level::HIGH));
    loop.run();

    CHECK(gotEdge);
    CHECK(gotLevel == gpio::Level::HIGH);
}

void testEventLoopTimerFiresOnce() {
    core::EventLoop loop;
    int fired = 0;
    loop.addTimer(std::chrono::milliseconds(10), /*repeat=*/false, [&] {
        ++fired;
        loop.stop();
    });
    loop.run();
    CHECK(fired == 1);
}

} // namespace

int main() {
    testOutputPinTracksDrivenLevel();
    testInputPinEdgeReachesEventLoop();
    testEventLoopTimerFiresOnce();

    const int failures = icom::testing::failureCount();
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
