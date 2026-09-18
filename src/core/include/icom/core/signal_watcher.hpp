#pragma once

#include "icom/core/event_loop.hpp"

#include <functional>
#include <initializer_list>

namespace icom::core {

// Turns signal delivery (SIGINT/SIGTERM for graceful shutdown, etc.) into
// ordinary fd readability via signalfd(2), so the daemon needs no
// async-signal-safety tightrope walk in a handler. The listed signals are
// blocked process-wide via sigprocmask -- they can then never interrupt
// arbitrary code -- and collected instead as a normal event on this fd,
// dispatched on the EventLoop's own thread like everything else.
class SignalWatcher {
public:
    // Construct on the EventLoop's thread before spawning any other
    // thread, so the blocked signal mask (which is inherited, not shared)
    // covers the whole process.
    SignalWatcher(EventLoop& loop, std::initializer_list<int> signals,
                  std::function<void(int signo)> on_signal);
    ~SignalWatcher();

    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;

private:
    EventLoop& loop_;
    int fd_ = -1;
};

} // namespace icom::core
