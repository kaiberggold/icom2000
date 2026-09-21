#include "icom/hw/pwm.hpp"
#include "icom/core/logger.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace icom::hw {

namespace {
core::Logger& log = core::getLogger("hw.pwm");
} // namespace

Pwm::Pwm(std::unique_ptr<gpio::OutputPin> pin, core::EventLoop& loop, std::chrono::milliseconds period)
    : pin_(std::move(pin)), loop_(loop), period_(period), dutyTime_(std::chrono::milliseconds::zero()) {
    log.debug("starting, period=" + std::to_string(period_.count()) + "ms");
    // Not written here: whichever thread runs this constructor isn't
    // necessarily loop_'s own, so the very first pin write (like every
    // other one) has to happen via startPeriod() on loop_'s thread too.
    loop_.post([this] { startPeriod(); });
}

Pwm::~Pwm() {
    // Block until the cancellation has actually run on loop_'s own
    // thread -- not just been posted -- so that by the time this
    // destructor returns (and pin_/this become invalid), no cycle
    // callback capturing `this` can still be pending. See the class
    // comment for why every method here funnels through post() instead
    // of touching loop_ directly from whatever thread calls it.
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    loop_.post([this, &m, &cv, &done] {
        loop_.removeTimer(pendingTimerId_);
        {
            std::lock_guard lock(m);
            done = true;
        }
        cv.notify_one();
    });
    std::unique_lock lock(m);
    cv.wait(lock, [&done] { return done; });
}

void Pwm::setDutyTime(std::chrono::milliseconds dutyTime) {
    dutyTime_.store(std::clamp(dutyTime, std::chrono::milliseconds::zero(), period_));
}

void Pwm::startPeriod() {
    const auto duty = dutyTime_.load();
    if (duty <= std::chrono::milliseconds::zero()) {
        pin_->write(gpio::Level::LOW);
        pendingTimerId_ = loop_.addTimer(period_, /*repeat=*/false, [this] { startPeriod(); });
    } else if (duty >= period_) {
        pin_->write(gpio::Level::HIGH);
        pendingTimerId_ = loop_.addTimer(period_, /*repeat=*/false, [this] { startPeriod(); });
    } else {
        pin_->write(gpio::Level::HIGH);
        pendingTimerId_ =
            loop_.addTimer(duty, /*repeat=*/false, [this, duty] { turnOffMidPeriod(duty); });
    }
}

void Pwm::turnOffMidPeriod(std::chrono::milliseconds dutyThisPeriod) {
    pin_->write(gpio::Level::LOW);
    pendingTimerId_ =
        loop_.addTimer(period_ - dutyThisPeriod, /*repeat=*/false, [this] { startPeriod(); });
}

} // namespace icom::hw
