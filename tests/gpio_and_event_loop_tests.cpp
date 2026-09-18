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

void test_output_pin_tracks_driven_level() {
    gpio::MockBackend backend;
    auto pin = backend.request_output(gpio::PinConfig{"mockchip0", 1, "test-output"}, gpio::Level::Low);

    CHECK(pin->driven_level() == gpio::Level::Low);
    pin->write(gpio::Level::High);
    CHECK(pin->driven_level() == gpio::Level::High);
}

void test_input_pin_edge_reaches_event_loop() {
    gpio::MockBackend backend;
    auto pin = backend.request_input(gpio::PinConfig{"mockchip0", 2, "test-input"}, gpio::Edge::Both);

    core::EventLoop loop;
    bool got_edge = false;
    gpio::Level got_level = gpio::Level::Low;

    loop.add_fd(pin->event_fd(), POLLIN, [&](short) {
        pin->consume_events([&](gpio::Level level, std::chrono::steady_clock::time_point) {
            got_edge = true;
            got_level = level;
        });
        loop.stop();
    });

    // Safety net: if the edge never reaches the loop, don't hang the suite.
    loop.add_timer(std::chrono::milliseconds(500), /*repeat=*/false, [&] { loop.stop(); });

    CHECK(gpio::inject_mock_edge(*pin, gpio::Level::High));
    loop.run();

    CHECK(got_edge);
    CHECK(got_level == gpio::Level::High);
}

void test_event_loop_timer_fires_once() {
    core::EventLoop loop;
    int fired = 0;
    loop.add_timer(std::chrono::milliseconds(10), /*repeat=*/false, [&] {
        ++fired;
        loop.stop();
    });
    loop.run();
    CHECK(fired == 1);
}

} // namespace

int main() {
    test_output_pin_tracks_driven_level();
    test_input_pin_edge_reaches_event_loop();
    test_event_loop_timer_fires_once();

    const int failures = icom::testing::failure_count();
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
