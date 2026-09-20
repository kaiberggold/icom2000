// Covers icom::hw::StatusLed against the mock GPIO backend: on()/off()
// track drivenLevel() correctly, and blinkNTimes() actually performs
// the right number of on/off transitions via the EventLoop rather than
// blocking the caller. See docs/ARCHITECTURE.md "Hardware layer".
#include "check.hpp"

#include "icom/core/event_loop.hpp"
#include "icom/gpio/mock_backend.hpp"
#include "icom/hw/status_led.hpp"

#include <chrono>

using namespace icom;

namespace {

// Wraps a mock OutputPin and counts every write() call, so a test can
// assert on the number of on/off transitions blinkNTimes() actually
// performed -- MockBackend's own OutputPin only exposes the *current*
// drivenLevel(), not a history of writes.
class CountingOutputPin : public gpio::OutputPin {
public:
    CountingOutputPin(std::unique_ptr<gpio::OutputPin> inner, int& writeCount)
        : inner_(std::move(inner)), writeCount_(writeCount) {}

    void write(gpio::Level level) override {
        ++writeCount_;
        inner_->write(level);
    }

    gpio::Level drivenLevel() const override { return inner_->drivenLevel(); }

private:
    std::unique_ptr<gpio::OutputPin> inner_;
    int& writeCount_;
};

void testStatusLedStartsOff() {
    gpio::MockBackend backend;
    auto pin = backend.requestOutput(gpio::PinConfig{"mockchip0", 23, "test-led"}, gpio::Level::HIGH);

    hw::StatusLed led(std::move(pin));
    CHECK(!led.isOn());
}

void testOnOffDriveThePinAndTrackState() {
    gpio::MockBackend backend;
    auto pin = backend.requestOutput(gpio::PinConfig{"mockchip0", 23, "test-led"}, gpio::Level::LOW);
    const gpio::OutputPin* raw = pin.get();

    hw::StatusLed led(std::move(pin));

    led.on();
    CHECK(led.isOn());
    CHECK(raw->drivenLevel() == gpio::Level::HIGH);

    led.off();
    CHECK(!led.isOn());
    CHECK(raw->drivenLevel() == gpio::Level::LOW);
}

void testBlinkNTimesIsANoopForZero() {
    gpio::MockBackend backend;
    auto pin = backend.requestOutput(gpio::PinConfig{"mockchip0", 23, "test-led"}, gpio::Level::LOW);

    hw::StatusLed led(std::move(pin));
    core::EventLoop loop;
    led.blinkNTimes(0, loop);

    // Nothing scheduled -- confirmed by not hanging: stop immediately via a
    // timer that would only be needed if blinkNTimes had (wrongly)
    // scheduled something.
    loop.addTimer(std::chrono::milliseconds(5), /*repeat=*/false, [&] { loop.stop(); });
    loop.run();
    CHECK(!led.isOn());
}

void testBlinkNTimesPerformsExactlyNOnOffCycles() {
    gpio::MockBackend backend;
    auto pin = backend.requestOutput(gpio::PinConfig{"mockchip0", 23, "test-led"}, gpio::Level::LOW);

    int writeCount = 0;
    auto countingPin = std::make_unique<CountingOutputPin>(std::move(pin), writeCount);
    const gpio::OutputPin* raw = countingPin.get();

    hw::StatusLed led(std::move(countingPin));
    writeCount = 0; // discard the constructor's own initial write(Low)
    core::EventLoop loop;

    constexpr unsigned BLINKS = 3;
    led.blinkNTimes(BLINKS, loop, std::chrono::milliseconds(1), std::chrono::milliseconds(1));

    // Safety net well past the ~2*BLINKS ms the sequence needs, in case a
    // regression breaks self-termination and would otherwise hang the
    // suite; also the only thing that stops the loop on the happy path,
    // since blinkNTimes() itself never calls loop.stop().
    loop.addTimer(std::chrono::milliseconds(200), /*repeat=*/false, [&] { loop.stop(); });
    loop.run();

    CHECK(writeCount == static_cast<int>(BLINKS) * 2); // one write() per on and per off
    CHECK(!led.isOn());
    CHECK(raw->drivenLevel() == gpio::Level::LOW);
}

} // namespace

int main() {
    testStatusLedStartsOff();
    testOnOffDriveThePinAndTrackState();
    testBlinkNTimesIsANoopForZero();
    testBlinkNTimesPerformsExactlyNOnOffCycles();

    const int failures = icom::testing::failureCount();
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
