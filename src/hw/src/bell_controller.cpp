#include "icom/hw/bell_controller.hpp"
#include "icom/core/logger.hpp"

namespace icom::hw {

namespace {
core::Logger& log = core::getLogger("hw.bell");
} // namespace

BellController::BellController(std::unique_ptr<gpio::OutputPin> pin) : pin_(std::move(pin)) {
    pin_->write(gpio::Level::LOW);
}

void BellController::ring() {
    pin_->write(gpio::Level::HIGH);
    ringing_ = true;
    log.info("ringing");
}

void BellController::silence() {
    pin_->write(gpio::Level::LOW);
    ringing_ = false;
    log.info("silenced");
}

bool BellController::isRinging() const { return ringing_; }

void BellController::ringFor(std::chrono::milliseconds duration, core::EventLoop& loop) {
    ring();
    loop.addTimer(duration, /*repeat=*/false, [this] { silence(); });
}

} // namespace icom::hw
