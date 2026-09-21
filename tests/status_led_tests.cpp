// Covers icom::hw::StatusLed against the mock GPIO backend: on()/off()
// track drivenLevel() correctly, and blinkNTimes() actually performs the
// right number of on/off transitions via a real background LoopThread
// (matching how daemon_main.cpp actually uses it) rather than blocking
// the caller. See docs/ARCHITECTURE.md "Software PWM" and StatusLed's own
// header comment for why blinkNTimes() against a LoopThread -- not the
// daemon's main loop -- is the case that actually needs covering here:
// ~StatusLed() requires whatever loop it last blinked on to still be
// actively running at destruction time, which only a LoopThread's own
// loop guarantees.
#include "check.hpp"

#include "icom/core/event_loop.hpp"
#include "icom/core/loop_thread.hpp"
#include "icom/gpio/mock_backend.hpp"
#include "icom/hw/status_led.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace icom;

namespace
{

// Wraps a mock OutputPin and counts every write() call, so a test can
// assert on the number of on/off transitions blinkNTimes() actually
// performed -- MockBackend's own OutputPin only exposes the *current*
// drivenLevel(), not a history of writes.
    class CountingOutputPin : public gpio::OutputPin
    {
    public:
        CountingOutputPin(std::unique_ptr<gpio::OutputPin> inner, int& writeCount)
            : inner_(std::move(inner)), writeCount_(writeCount) {}

        void write(gpio::Level level) override
        {
            ++writeCount_;
            inner_->write(level);
        }

        gpio::Level drivenLevel() const override { return inner_->drivenLevel(); }

    private:
        std::unique_ptr<gpio::OutputPin> inner_;
        int& writeCount_;
    };

    void testStatusLedStartsOff()
    {
        gpio::MockBackend backend;
        auto pin = backend.requestOutput(gpio::PinConfig{"mockchip0", 23, "test-led"}, gpio::Level::HIGH);

        hw::StatusLed led(std::move(pin));
        CHECK(!led.isOn());
    }

    void testOnOffDriveThePinAndTrackState()
    {
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

    void testBlinkNTimesIsANoopForZero()
    {
        gpio::MockBackend backend;
        auto pin = backend.requestOutput(gpio::PinConfig{"mockchip0", 23, "test-led"}, gpio::Level::LOW);

        hw::StatusLed led(std::move(pin));
        // Never run() -- blinkNTimes(0, ...) must return without ever
        // touching `loop` at all (confirmed by this test not hanging in
        // ~StatusLed(), which only waits on a loop it actually used).
        core::EventLoop loop;
        led.blinkNTimes(0, loop);

        CHECK(!led.isOn());
    }

    void testBlinkNTimesPerformsExactlyNOnOffCycles()
    {
        gpio::MockBackend backend;
        auto pin = backend.requestOutput(gpio::PinConfig{"mockchip0", 23, "test-led"}, gpio::Level::LOW);

        int writeCount = 0;
        auto countingPin = std::make_unique<CountingOutputPin>(std::move(pin), writeCount);
        const gpio::OutputPin* raw = countingPin.get();

        // Declared before `led` (so destroyed after it): ~StatusLed() needs
        // this loop still actively running when it tears down, which only
        // holds while `background`'s own thread is still alive.
        core::LoopThread background;
        hw::StatusLed led(std::move(countingPin));
        writeCount = 0; // discard the constructor's own initial write(Low)

        constexpr unsigned BLINKS = 3;

        // blinkNTimes() must be called from `background`'s own thread (per
        // its contract), so post() it there; it doesn't itself report
        // completion, so a trailing timer -- scheduled well after the
        // ~2*BLINKS ms the sequence needs -- doubles as the completion signal
        // and the safety net that keeps this test from hanging if a
        // regression breaks self-termination.
        std::mutex m;
        std::condition_variable cv;
        bool sequenceDone = false;
        background.loop().post([&]
        {
            led.blinkNTimes(BLINKS, background.loop(), std::chrono::milliseconds(1),
                            std::chrono::milliseconds(1));
            background.loop().addTimer(std::chrono::milliseconds(100), /*repeat=*/false, [&] {
                std::lock_guard lock(m);
                sequenceDone = true;
                cv.notify_one();
            });
        });

        std::unique_lock lock(m);
        const bool finished = cv.wait_for(lock, std::chrono::seconds(2), [&] { return sequenceDone; });
        lock.unlock();

        CHECK(finished);
        CHECK(writeCount == static_cast<int>(BLINKS) * 2); // one write() per on and per off
        CHECK(!led.isOn());
        CHECK(raw->drivenLevel() == gpio::Level::LOW);
    }

} // namespace

int main()
{
    testStatusLedStartsOff();
    testOnOffDriveThePinAndTrackState();
    testBlinkNTimesIsANoopForZero();
    testBlinkNTimesPerformsExactlyNOnOffCycles();

    const int failures = icom::testing::failureCount();
    if (failures > 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
