#include "icom/core/event_loop.hpp"
#include "icom/core/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace icom::core {

namespace {

void throw_errno(std::string_view what) {
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

} // namespace

struct EventLoop::Impl {
    std::vector<pollfd> pollfds;
    std::unordered_map<int, FdCallback> fd_callbacks;
    std::unordered_map<TimerId, int> timer_fd_by_id;
    TimerId next_timer_id = 1;
    int wake_fd = -1;
    bool running = false;

    void add_fd_locked(int fd, short events, FdCallback cb) {
        pollfds.push_back(pollfd{fd, events, 0});
        fd_callbacks.emplace(fd, std::move(cb));
    }

    void remove_fd_locked(int fd) {
        std::erase_if(pollfds, [fd](const pollfd& p) { return p.fd == fd; });
        fd_callbacks.erase(fd);
    }
};

EventLoop::EventLoop() : impl_(std::make_unique<Impl>()) {
    impl_->wake_fd = eventfd(0, EFD_NONBLOCK);
    if (impl_->wake_fd < 0) {
        throw_errno("eventfd() failed");
    }
    impl_->add_fd_locked(impl_->wake_fd, POLLIN, [this](short) {
        std::uint64_t drain = 0;
        // Just a wakeup signal; the loop re-checks `running` on its own.
        while (::read(impl_->wake_fd, &drain, sizeof(drain)) > 0) {
        }
    });
}

EventLoop::~EventLoop() {
    if (impl_->wake_fd >= 0) {
        ::close(impl_->wake_fd);
    }
    for (auto& [id, fd] : impl_->timer_fd_by_id) {
        ::close(fd);
    }
}

void EventLoop::add_fd(int fd, short events, FdCallback callback) {
    impl_->add_fd_locked(fd, events, std::move(callback));
}

void EventLoop::remove_fd(int fd) { impl_->remove_fd_locked(fd); }

EventLoop::TimerId EventLoop::add_timer(std::chrono::milliseconds interval, bool repeat,
                                         TimerCallback callback) {
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd < 0) {
        throw_errno("timerfd_create() failed");
    }

    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(interval);
    const auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(interval - secs);

    itimerspec spec{};
    spec.it_value.tv_sec = secs.count();
    spec.it_value.tv_nsec = nsecs.count();
    if (repeat) {
        spec.it_interval = spec.it_value;
    }
    if (timerfd_settime(fd, 0, &spec, nullptr) != 0) {
        ::close(fd);
        throw_errno("timerfd_settime() failed");
    }

    const TimerId id = impl_->next_timer_id++;
    impl_->timer_fd_by_id.emplace(id, fd);

    impl_->add_fd_locked(fd, POLLIN, [this, fd, repeat, cb = std::move(callback)](short) {
        std::uint64_t expirations = 0;
        if (::read(fd, &expirations, sizeof(expirations)) <= 0) {
            return; // spurious wakeup or already disarmed
        }
        cb();
        if (!repeat) {
            remove_fd(fd);
            ::close(fd);
            std::erase_if(impl_->timer_fd_by_id, [fd](const auto& kv) { return kv.second == fd; });
        }
    });

    return id;
}

void EventLoop::remove_timer(TimerId id) {
    auto it = impl_->timer_fd_by_id.find(id);
    if (it == impl_->timer_fd_by_id.end()) {
        return;
    }
    const int fd = it->second;
    remove_fd(fd);
    ::close(fd);
    impl_->timer_fd_by_id.erase(it);
}

void EventLoop::run(std::stop_token token) {
    impl_->running = true;

    std::stop_callback wake_on_stop(token, [this] { stop(); });

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
            log_error(std::string("poll() failed: ") + std::strerror(errno));
            break;
        }

        for (const auto& pfd : snapshot) {
            if (pfd.revents == 0) {
                continue;
            }
            auto it = impl_->fd_callbacks.find(pfd.fd);
            if (it == impl_->fd_callbacks.end()) {
                continue; // removed by an earlier callback in this same batch
            }
            // Copy out rather than invoking through the map reference: a
            // callback is allowed to remove_fd() its own fd (one-shot
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
    (void)::write(impl_->wake_fd, &one, sizeof(one));
}

} // namespace icom::core
