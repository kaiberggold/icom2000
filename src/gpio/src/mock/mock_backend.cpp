#include "icom/gpio/mock_backend.hpp"
#include "icom/core/logger.hpp"

#include <deque>
#include <mutex>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

namespace icom::gpio {

namespace {

class MockOutputPin final : public OutputPin {
public:
    explicit MockOutputPin(PinConfig config, Level initial)
        : config_(std::move(config)), level_(initial) {}

    void write(Level level) override { level_ = level; }
    Level driven_level() const override { return level_; }

private:
    PinConfig config_;
    Level level_;
};

class MockInputPin final : public InputPin {
public:
    explicit MockInputPin(PinConfig config) : config_(std::move(config)) {
        event_fd_ = eventfd(0, EFD_NONBLOCK);
    }

    ~MockInputPin() override {
        if (event_fd_ >= 0) {
            ::close(event_fd_);
        }
    }

    Level read() const override {
        std::lock_guard lock(mutex_);
        return level_;
    }

    int event_fd() const override { return event_fd_; }

    void consume_events(const EdgeCallback& callback) override {
        std::uint64_t drain = 0;
        (void)::read(event_fd_, &drain, sizeof(drain));

        std::deque<std::pair<Level, std::chrono::steady_clock::time_point>> pending;
        {
            std::lock_guard lock(mutex_);
            pending.swap(pending_events_);
        }
        for (const auto& [level, at] : pending) {
            callback(level, at);
        }
    }

    // Test-only entry point, reached via inject_mock_edge().
    void inject(Level level) {
        {
            std::lock_guard lock(mutex_);
            level_ = level;
            pending_events_.emplace_back(level, std::chrono::steady_clock::now());
        }
        const std::uint64_t one = 1;
        (void)::write(event_fd_, &one, sizeof(one));
    }

private:
    PinConfig config_;
    int event_fd_ = -1;
    mutable std::mutex mutex_;
    Level level_ = Level::Low;
    std::deque<std::pair<Level, std::chrono::steady_clock::time_point>> pending_events_;
};

} // namespace

std::unique_ptr<OutputPin> MockBackend::request_output(const PinConfig& config, Level initial) {
    core::log_debug("mock gpio: requesting output " + config.chip + ":" + std::to_string(config.line) +
                     " (" + config.consumer + ")");
    return std::make_unique<MockOutputPin>(config, initial);
}

std::unique_ptr<InputPin> MockBackend::request_input(const PinConfig& config, Edge /*edge*/) {
    core::log_debug("mock gpio: requesting input " + config.chip + ":" + std::to_string(config.line) +
                     " (" + config.consumer + ")");
    return std::make_unique<MockInputPin>(config);
}

bool inject_mock_edge(InputPin& pin, Level level) {
    if (auto* mock = dynamic_cast<MockInputPin*>(&pin)) {
        mock->inject(level);
        return true;
    }
    return false;
}

} // namespace icom::gpio
