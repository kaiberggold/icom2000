#include "icom/hw/status_led.hpp"
#include "icom/core/logger.hpp"

#include <condition_variable>
#include <mutex>

namespace icom::hw
{

    namespace
    {
        core::Logger& log = core::getLogger("hw.status_led");
    } // namespace

    StatusLed::StatusLed(std::unique_ptr<gpio::IOutputPin> pin) : pin_(std::move(pin))
    {
        pin_->write(gpio::Level::LOW);
    }

    StatusLed::~StatusLed()
    {
        core::EventLoop* loop = blinkLoop_.load();
        if (loop == nullptr)
        {
            return; // blinkNTimes() was never called -- nothing to cancel
        }
        // Block until the cancellation has actually run on loop's own thread
        // -- not just been posted -- so that by the time this destructor
        // returns (and `this` becomes invalid), blinkStep() can no longer
        // read anything through it. See the class comment.
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        loop->post([this, &m, &cv, &done]
        {
            cancelled_ = true;
            {
                std::lock_guard lock(m);
                done = true;
            }
            cv.notify_one();
        });
        std::unique_lock lock(m);
        cv.wait(lock, [&done] { return done; });
    }

    void StatusLed::on()
    {
        pin_->write(gpio::Level::HIGH);
        on_.store(true);
    }

    void StatusLed::off()
    {
        pin_->write(gpio::Level::LOW);
        on_.store(false);
    }

    void StatusLed::blinkNTimes(unsigned times, core::EventLoop& loop, std::chrono::milliseconds onDuration,
                                std::chrono::milliseconds offDuration)
    {
        if (times == 0)
        {
            return;
        }
        log.debug("blinking " + std::to_string(times) + " time(s), " + std::to_string(onDuration.count()) +
                  " ms on / " + std::to_string(offDuration.count()) + " ms off");
        blinkLoop_.store(&loop);
        remaining_ = times;
        onDuration_ = onDuration;
        offDuration_ = offDuration;
        on();
        loop.addTimer(onDuration, /*repeat=*/false, [this] { blinkStep(); });
    }

    void StatusLed::blinkStep()
    {
        if (cancelled_)
        {
            return; // ~StatusLed() is tearing this down; touch nothing further
        }
        core::EventLoop& loop = *blinkLoop_.load();
        if (isOn())
        {
            off();
            if (--remaining_ == 0)
            {
                return; // sequence complete
            }
            loop.addTimer(offDuration_, /*repeat=*/false, [this] { blinkStep(); });
        }
        else
        {
            on();
            loop.addTimer(onDuration_, /*repeat=*/false, [this] { blinkStep(); });
        }
    }

} // namespace icom::hw
