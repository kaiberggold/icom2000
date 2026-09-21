#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>

namespace icom::core {

// Single-threaded reactor: everything given to one EventLoop (GPIO edge
// events, the control socket, timers) is a file descriptor that gets
// multiplexed with poll() on whichever one thread calls run(). See
// docs/ARCHITECTURE.md "Run model" for why this beats a thread-per-
// component design on a single-core Pi Zero, and "Software PWM" for the
// one deliberate exception: a second EventLoop, on a second thread
// (LoopThread), for periodic work the main one shouldn't have to wait its
// turn for. post() (below) is what makes talking to that second loop from
// the main thread safe.
//
// poll() rather than epoll(): the fd count here is a handful (gpio chip,
// listening socket, a few client connections, a couple of timers), so
// epoll's O(1) registration bookkeeping buys nothing, and poll()'s flat
// vector-of-pollfd is simpler to reason about and to unit-test. Revisit if
// this ever needs to scale to hundreds of connections, which a home
// intercom will not.
class EventLoop {
public:
    // revents: the poll(2) events that woke the loop for this fd (POLLIN, POLLHUP, ...).
    using FdCallback = std::function<void(short revents)>;
    using TimerCallback = std::function<void()>;
    using TimerId = std::uint64_t;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // `events` is a poll(2) request mask, typically POLLIN. The callback is
    // invoked from run() on this same thread -- no locking needed inside it.
    void addFd(int fd, short events, FdCallback callback);
    void removeFd(int fd);

    // Backed by timerfd_create(2), so timers are just more fds to the
    // reactor rather than a separate subsystem.
    TimerId addTimer(std::chrono::milliseconds interval, bool repeat, TimerCallback callback);
    void removeTimer(TimerId id);

    // The one EventLoop operation safe to call from a thread OTHER than
    // whichever one is running this loop's run() (see LoopThread,
    // icom/core/loop_thread.hpp, for the intended use: a second EventLoop
    // on its own dedicated thread for timing-sensitive periodic work like
    // software PWM). `fn` runs on the loop's own thread, asynchronously --
    // post() itself returns immediately, before `fn` has necessarily run.
    // Every other method here (addFd, addTimer, ...) is NOT thread-safe;
    // call them only from the loop's own thread, which is exactly what a
    // callback running via post() (or any other EventLoop callback) is.
    void post(std::function<void()> fn);

    // Blocks, dispatching callbacks, until stop() is called or `token` is
    // cancelled. Safe to call stop() from another thread (e.g. a signal
    // handler thread) -- it wakes the loop via a self-pipe.
    void run(std::stop_token token = {});
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace icom::core
