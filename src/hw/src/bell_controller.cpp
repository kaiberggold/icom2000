#include "icom/hw/bell_controller.hpp"
#include "icom/core/logger.hpp"

namespace icom::hw {

namespace {
core::Logger& kLog = core::get_logger("hw.bell");
} // namespace

BellController::BellController(std::unique_ptr<gpio::OutputPin> pin) : pin_(std::move(pin)) {
    pin_->write(gpio::Level::Low);
}

void BellController::ring() {
    pin_->write(gpio::Level::High);
    ringing_ = true;
    kLog.info("ringing");
}

void BellController::silence() {
    pin_->write(gpio::Level::Low);
    ringing_ = false;
    kLog.info("silenced");
}

bool BellController::is_ringing() const { return ringing_; }

void BellController::ring_for(std::chrono::milliseconds duration, core::EventLoop& loop) {
    ring();
    loop.add_timer(duration, /*repeat=*/false, [this] { silence(); });
}

} // namespace icom::hw
