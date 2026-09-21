// Exercises the pieces a dev host can actually test without real
// hardware: the mock GPIO backend, the EventLoop reactor it's designed to
// plug into (including post(), EventLoop's one thread-safe operation),
// and LoopThread, the EventLoop-on-its-own-thread wrapper icom::hw::Pwm
// and StatusLed's background blinking are built on. Together the GPIO
// pieces stand in for what would otherwise need a physical GPIO chip --
// this is the payoff of the OutputPin/InputPin interface split in
// icom/gpio/digital_pin.hpp.
#include "check.hpp"

#include "icom/core/event_loop.hpp"
#include "icom/core/loop_thread.hpp"
#include "icom/gpio/mock_backend.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

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

void testEventLoopPostRunsOnTheLoopsOwnThread() {
    core::EventLoop loop;
    bool ran = false;
    std::thread::id postedFromThread = std::this_thread::get_id();
    std::thread::id ranOnThread{};

    // post() itself is called from THIS thread (the test's), matching how
    // every real caller uses it (from a thread other than the one running
    // the loop) -- run() is what's on this same thread, on purpose, so
    // this test doesn't need a second thread just to prove post() got the
    // callback to run inside run()'s dispatch.
    loop.post([&] {
        ran = true;
        ranOnThread = std::this_thread::get_id();
        loop.stop();
    });
    loop.addTimer(std::chrono::milliseconds(500), /*repeat=*/false, [&] { loop.stop(); });
    loop.run();

    CHECK(ran);
    CHECK(ranOnThread == postedFromThread); // single-threaded here, but confirms it ran at all
}

void testLoopThreadPostRunsOnItsOwnBackgroundThread() {
    core::LoopThread background;
    const std::thread::id testThread = std::this_thread::get_id();

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::thread::id ranOnThread{};

    background.loop().post([&] {
        ranOnThread = std::this_thread::get_id();
        {
            std::lock_guard lock(m);
            done = true;
        }
        cv.notify_one();
    });

    std::unique_lock lock(m);
    const bool finished = cv.wait_for(lock, std::chrono::seconds(2), [&] { return done; });

    CHECK(finished);
    CHECK(ranOnThread != testThread); // genuinely a different (background) thread
}

} // namespace

int main() {
    testOutputPinTracksDrivenLevel();
    testInputPinEdgeReachesEventLoop();
    testEventLoopTimerFiresOnce();
    testEventLoopPostRunsOnTheLoopsOwnThread();
    testLoopThreadPostRunsOnItsOwnBackgroundThread();

    const int failures = icom::testing::failureCount();
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
