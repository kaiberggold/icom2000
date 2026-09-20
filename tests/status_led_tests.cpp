// Covers icom::hw::StatusLed against the mock GPIO backend: on()/off()
// track driven_level() correctly, and blink_n_times() actually performs
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
// assert on the number of on/off transitions blink_n_times() actually
// performed -- MockBackend's own OutputPin only exposes the *current*
// driven_level(), not a history of writes.
class CountingOutputPin : public gpio::OutputPin {
public:
    CountingOutputPin(std::unique_ptr<gpio::OutputPin> inner, int& write_count)
        : inner_(std::move(inner)), write_count_(write_count) {}

    void write(gpio::Level level) override {
        ++write_count_;
        inner_->write(level);
    }

    gpio::Level driven_level() const override { return inner_->driven_level(); }

private:
    std::unique_ptr<gpio::OutputPin> inner_;
    int& write_count_;
};

void test_status_led_starts_off() {
    gpio::MockBackend backend;
    auto pin = backend.request_output(gpio::PinConfig{"mockchip0", 23, "test-led"}, gpio::Level::High);

    hw::StatusLed led(std::move(pin));
    CHECK(!led.is_on());
}

void test_on_off_drive_the_pin_and_track_state() {
    gpio::MockBackend backend;
    auto pin = backend.request_output(gpio::PinConfig{"mockchip0", 23, "test-led"}, gpio::Level::Low);
    const gpio::OutputPin* raw = pin.get();

    hw::StatusLed led(std::move(pin));

    led.on();
    CHECK(led.is_on());
    CHECK(raw->driven_level() == gpio::Level::High);

    led.off();
    CHECK(!led.is_on());
    CHECK(raw->driven_level() == gpio::Level::Low);
}

void test_blink_n_times_is_a_noop_for_zero() {
    gpio::MockBackend backend;
    auto pin = backend.request_output(gpio::PinConfig{"mockchip0", 23, "test-led"}, gpio::Level::Low);

    hw::StatusLed led(std::move(pin));
    core::EventLoop loop;
    led.blink_n_times(0, loop);

    // Nothing scheduled -- confirmed by not hanging: stop immediately via a
    // timer that would only be needed if blink_n_times had (wrongly)
    // scheduled something.
    loop.add_timer(std::chrono::milliseconds(5), /*repeat=*/false, [&] { loop.stop(); });
    loop.run();
    CHECK(!led.is_on());
}

void test_blink_n_times_performs_exactly_n_on_off_cycles() {
    gpio::MockBackend backend;
    auto pin = backend.request_output(gpio::PinConfig{"mockchip0", 23, "test-led"}, gpio::Level::Low);

    int write_count = 0;
    auto counting_pin = std::make_unique<CountingOutputPin>(std::move(pin), write_count);
    const gpio::OutputPin* raw = counting_pin.get();

    hw::StatusLed led(std::move(counting_pin));
    write_count = 0; // discard the constructor's own initial write(Low)
    core::EventLoop loop;

    constexpr unsigned kBlinks = 3;
    led.blink_n_times(kBlinks, loop, std::chrono::milliseconds(1), std::chrono::milliseconds(1));

    // Safety net well past the ~2*kBlinks ms the sequence needs, in case a
    // regression breaks self-termination and would otherwise hang the
    // suite; also the only thing that stops the loop on the happy path,
    // since blink_n_times() itself never calls loop.stop().
    loop.add_timer(std::chrono::milliseconds(200), /*repeat=*/false, [&] { loop.stop(); });
    loop.run();

    CHECK(write_count == static_cast<int>(kBlinks) * 2); // one write() per on and per off
    CHECK(!led.is_on());
    CHECK(raw->driven_level() == gpio::Level::Low);
}

} // namespace

int main() {
    test_status_led_starts_off();
    test_on_off_drive_the_pin_and_track_state();
    test_blink_n_times_is_a_noop_for_zero();
    test_blink_n_times_performs_exactly_n_on_off_cycles();

    const int failures = icom::testing::failure_count();
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
