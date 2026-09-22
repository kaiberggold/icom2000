// Covers icom::hw::Pwm against the mock GPIO backend and a real
// icom::core::LoopThread -- Pwm's own construction/destruction require an
// actively-running background loop (see that class's header comment), so
// unlike most of this test suite there's no single-threaded stand-in for
// "just call run() once and inspect state" here.
#include "check.hpp"

#include "icom/core/loop_thread.hpp"
#include "icom/gpio/mock_backend.hpp"
#include "icom/hw/pwm.hpp"

#include <chrono>
#include <thread>

using namespace icom;
using namespace std::chrono_literals;

namespace
{

    void testPwmStartsAtZeroDutyWithTheConfiguredPeriod()
    {
        gpio::MockBackend backend;
        core::LoopThread background;
        hw::Pwm pwm(backend.requestOutput(gpio::PinConfig{"mockchip0", 1, "test-pwm"}, gpio::Level::LOW),
                    background.loop(), 20ms);

        CHECK(pwm.period() == 20ms);
        CHECK(pwm.dutyTime() == 0ms);
    }

    void testSetDutyTimeClampsToZeroAndPeriod()
    {
        gpio::MockBackend backend;
        core::LoopThread background;
        hw::Pwm pwm(backend.requestOutput(gpio::PinConfig{"mockchip0", 1, "test-pwm"}, gpio::Level::LOW),
                    background.loop(), 20ms);

        pwm.setDutyTime(100ms); // > period
        CHECK(pwm.dutyTime() == 20ms);

        pwm.setDutyTime(-5ms); // < 0
        CHECK(pwm.dutyTime() == 0ms);
    }

    void testZeroDutyKeepsThePinContinuouslyLow()
    {
        gpio::MockBackend backend;
        auto pin = backend.requestOutput(gpio::PinConfig{"mockchip0", 1, "test-pwm"}, gpio::Level::HIGH);
        const gpio::IOutputPin* raw = pin.get();

        core::LoopThread background;
        hw::Pwm pwm(std::move(pin), background.loop(), 5ms); // duty stays 0 (the default)

        bool sawHigh = false;
        for (int i = 0; i < 10; ++i)
        {
            std::this_thread::sleep_for(5ms);
            if (raw->drivenLevel() == gpio::Level::HIGH)
            {
                sawHigh = true;
            }
        }

        CHECK(!sawHigh);
        CHECK(raw->drivenLevel() == gpio::Level::LOW);
    }

    void testFullDutyKeepsThePinContinuouslyHigh()
    {
        gpio::MockBackend backend;
        auto pin = backend.requestOutput(gpio::PinConfig{"mockchip0", 1, "test-pwm"}, gpio::Level::LOW);
        const gpio::IOutputPin* raw = pin.get();

        core::LoopThread background;
        hw::Pwm pwm(std::move(pin), background.loop(), 5ms);
        pwm.setDutyTime(5ms); // == period: full on

        // Give the first period a moment to land before sampling.
        std::this_thread::sleep_for(10ms);

        bool sawLow = false;
        for (int i = 0; i < 10; ++i)
        {
            std::this_thread::sleep_for(5ms);
            if (raw->drivenLevel() == gpio::Level::LOW)
            {
                sawLow = true;
            }
        }

        CHECK(!sawLow);
        CHECK(raw->drivenLevel() == gpio::Level::HIGH);
    }

    void testPartialDutyTogglesThePin()
    {
        gpio::MockBackend backend;
        auto pin = backend.requestOutput(gpio::PinConfig{"mockchip0", 1, "test-pwm"}, gpio::Level::LOW);
        const gpio::IOutputPin* raw = pin.get();

        core::LoopThread background;
        hw::Pwm pwm(std::move(pin), background.loop(), 10ms);
        pwm.setDutyTime(5ms); // 50%

        bool sawHigh = false;
        bool sawLow = false;
        for (int i = 0; i < 20; ++i)
        {
            std::this_thread::sleep_for(5ms);
            if (raw->drivenLevel() == gpio::Level::HIGH)
            {
                sawHigh = true;
            }
            else
            {
                sawLow = true;
            }
        }

        CHECK(sawHigh);
        CHECK(sawLow);
    }

} // namespace

int main()
{
    testPwmStartsAtZeroDutyWithTheConfiguredPeriod();
    testSetDutyTimeClampsToZeroAndPeriod();
    testZeroDutyKeepsThePinContinuouslyLow();
    testFullDutyKeepsThePinContinuouslyHigh();
    testPartialDutyTogglesThePin();

    const int failures = icom::testing::failureCount();
    if (failures > 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
