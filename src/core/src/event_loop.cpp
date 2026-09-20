#include "icom/core/event_loop.hpp"
#include "icom/core/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace icom::core {

namespace {

Logger& log = getLogger("core.event_loop");

void throwErrno(std::string_view what) {
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

// The one place a chrono duration gets translated into the POSIX
// {seconds, nanoseconds} pair timerfd_settime() wants, so the narrowing
// casts (chrono's rep is a 64-bit count on every platform this targets;
// timespec::tv_sec/tv_nsec are not, notably tv_nsec is a 32-bit `long` on
// ARM32) live in one named function instead of two unchecked field
// assignments at the call site.
timespec toTimespec(std::chrono::nanoseconds duration) {
    using namespace std::chrono;
    const auto secs = duration_cast<seconds>(duration);
    return timespec{static_cast<std::time_t>(secs.count()),
                    static_cast<long>((duration - secs).count())};
}

} // namespace

struct EventLoop::Impl {
    std::vector<pollfd> pollfds;
    std::unordered_map<int, FdCallback> fdCallbacks;
    std::unordered_map<TimerId, int> timerFdById;
    TimerId nextTimerId = 1;
    int wakeFd = -1;
    bool running = false;

    void addFdLocked(int fd, short events, FdCallback cb) {
        pollfds.push_back(pollfd{fd, events, 0});
        fdCallbacks.emplace(fd, std::move(cb));
    }

    void removeFdLocked(int fd) {
        std::erase_if(pollfds, [fd](const pollfd & p) { return p.fd == fd; });
        fdCallbacks.erase(fd);
    }
};

EventLoop::EventLoop() : impl_(std::make_unique<Impl>()) {
    impl_->wakeFd = eventfd(0, EFD_NONBLOCK);
    if (impl_->wakeFd < 0) {
        throwErrno("eventfd() failed");
    }
    impl_->addFdLocked(impl_->wakeFd, POLLIN, [this](short) {
        std::uint64_t drain = 0;
        // Just a wakeup signal; the loop re-checks `running` on its own.
        while (::read(impl_->wakeFd, &drain, sizeof(drain)) > 0) {
        }
    });
}

EventLoop::~EventLoop() {
    if (impl_->wakeFd >= 0) {
        ::close(impl_->wakeFd);
    }
    for (auto& [id, fd] : impl_->timerFdById) {
        ::close(fd);
    }
}

void EventLoop::addFd(int fd, short events, FdCallback callback) {
    impl_->addFdLocked(fd, events, std::move(callback));
}

void EventLoop::removeFd(int fd) { impl_->removeFdLocked(fd); }

EventLoop::TimerId EventLoop::addTimer(std::chrono::milliseconds interval, bool repeat,
                                       TimerCallback callback) {
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd < 0) {
        throwErrno("timerfd_create() failed");
    }

    itimerspec spec{};
    spec.it_value = toTimespec(std::chrono::duration_cast<std::chrono::nanoseconds>(interval));
    if (repeat) {
        spec.it_interval = spec.it_value;
    }
    if (timerfd_settime(fd, 0, &spec, nullptr) != 0) {
        ::close(fd);
        throwErrno("timerfd_settime() failed");
    }

    const TimerId id = impl_->nextTimerId++;
    impl_->timerFdById.emplace(id, fd);

    impl_->addFdLocked(fd, POLLIN, [this, fd, repeat, cb = std::move(callback)](short) {
        std::uint64_t expirations = 0;
        if (::read(fd, &expirations, sizeof(expirations)) <= 0) {
            return; // spurious wakeup or already disarmed
        }
        cb();
        if (!repeat) {
            removeFd(fd);
            ::close(fd);
            std::erase_if(impl_->timerFdById, [fd](const auto & kv) { return kv.second == fd; });
        }
    });

    return id;
}

void EventLoop::removeTimer(TimerId id) {
    auto it = impl_->timerFdById.find(id);
    if (it == impl_->timerFdById.end()) {
        return;
    }
    const int fd = it->second;
    removeFd(fd);
    ::close(fd);
    impl_->timerFdById.erase(it);
}

void EventLoop::run(std::stop_token token) {
    impl_->running = true;

    std::stop_callback wakeOnStop(token, [this] { stop(); });

    while (impl_->running) {
        // Snapshot: a callback may add/remove fds (e.g. accept() adding a
        // new client, or a one-shot timer removing itself), which would
        // invalidate iterators/indices into the live vector mid-dispatch.
        std::vector<pollfd> snapshot = impl_->pollfds;

        const int rc = ::poll(snapshot.data(), snapshot.size(), -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            log.error(std::string("poll() failed: ") + std::strerror(errno));
            break;
        }

        for (const auto& pfd : snapshot) {
            if (pfd.revents == 0) {
                continue;
            }
            auto it = impl_->fdCallbacks.find(pfd.fd);
            if (it == impl_->fdCallbacks.end()) {
                continue; // removed by an earlier callback in this same batch
            }
            // Copy out rather than invoking through the map reference: a
            // callback is allowed to removeFd() its own fd (one-shot
            // timers do exactly this), which erases -- and so destroys --
            // the std::function it->second refers to. Invoking through a
            // reference into an object that gets destroyed mid-call is
            // use-after-free; this local copy keeps the closure (and
            // everything it captured, including `this`-by-value captures)
            // alive for the duration of the call regardless of what the
            // callback does to the map.
            FdCallback callback = it->second;
            callback(pfd.revents);
            if (!impl_->running) {
                break;
            }
        }
    }
}

void EventLoop::stop() {
    impl_->running = false;
    std::uint64_t one = 1;
    // Best-effort: if this races with destruction the fd may already be
    // gone, which is fine -- there is nothing left to wake.
    (void)::write(impl_->wakeFd, &one, sizeof(one));
}

} // namespace icom::core
