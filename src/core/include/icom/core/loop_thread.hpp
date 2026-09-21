#pragma once

#include "icom/core/event_loop.hpp"

#include <thread>

namespace icom::core {

// A second EventLoop, on its own dedicated thread, for periodic work that
// can't share the daemon's main reactor thread without risking jitter --
// the same reasoning docs/ARCHITECTURE.md "Audio boundary" already gives
// for why real audio I/O would need its own thread(s). Software PWM
// (icom::hw::Pwm) is the first real user: a fixed, fast on/off cycle that
// would otherwise compete with the main loop's poll() dispatch for every
// IPC command, GPIO edge, and timer the rest of the daemon handles.
//
// Anything else that wants "run this repeatedly, precisely, without the
// rest of the daemon in the way" can share this same LoopThread and its
// loop() rather than spinning up a thread of its own -- e.g. StatusLed's
// blinkNTimes() takes any EventLoop&, so pointing it at loop() instead of
// the main loop is enough to move LED blinking here too.
//
// Only loop()'s post() is safe to call from outside this thread (see
// EventLoop::post()) -- that's the intended way in: post a lambda that
// does the real work (construct a Pwm, call blinkNTimes(), ...) rather
// than calling any other EventLoop method directly from another thread.
class LoopThread {
public:
    LoopThread();
    ~LoopThread();

    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;

    EventLoop& loop() { return loop_; }

private:
    // Declaration order matters: loop_ must finish constructing before
    // thread_'s constructor runs (it immediately starts calling
    // loop_.run() on the new thread), and members initialize in
    // declaration order regardless of member-initializer-list order.
    EventLoop loop_;
    std::jthread thread_;
};

} // namespace icom::core
