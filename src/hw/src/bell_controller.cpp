#include "icom/hw/bell_controller.hpp"
#include "icom/core/logger.hpp"

#include <algorithm>

namespace icom::hw
{

    namespace
    {
        core::Logger& log = core::getLogger("hw.bell");
    } // namespace

    BellController::BellController(std::unique_ptr<Pwm> pwm, std::chrono::milliseconds ringDutyTime)
        : pwm_(std::move(pwm)),
          ringDutyTime_(std::clamp(ringDutyTime, std::chrono::milliseconds::zero(), pwm_->period())) {}

    void BellController::ring()
    {
        pwm_->setDutyTime(ringDutyTime_);
        ringing_ = true;
        log.info("ringing (duty=" + std::to_string(ringDutyTime_.count()) + "/" +
                 std::to_string(pwm_->period().count()) + "ms)");
    }

    void BellController::silence()
    {
        pwm_->setDutyTime(std::chrono::milliseconds::zero());
        ringing_ = false;
        log.info("silenced");
    }

    bool BellController::isRinging() const { return ringing_; }

    void BellController::ringFor(std::chrono::milliseconds duration, core::EventLoop& loop)
    {
        ring();
        loop.addTimer(duration, /*repeat=*/false, [this] { silence(); });
    }

} // namespace icom::hw
