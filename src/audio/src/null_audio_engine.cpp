#include "icom/audio/audio_engine.hpp"
#include "icom/config/station_registry.hpp"
#include "icom/core/logger.hpp"

#include <utility>
#include <vector>

namespace icom::audio {

namespace {

core::Logger& kLog = core::get_logger("audio");

class NullAudioEngine final : public AudioEngine {
public:
    explicit NullAudioEngine(std::vector<config::StationConfig> stations) : stations_(std::move(stations)) {}

    void start() override {
        // Named devices are already sitting in `stations_`, ready for a
        // real ALSA implementation to open them by name -- logged here
        // mainly so that fact is visible/testable before one exists.
        for (const auto& station : stations_) {
            kLog.debug("station \"" + station.name + "\" would open capture=" + station.capture_device +
                       ", playback=" + station.playback_device);
        }
        kLog.warn("NullAudioEngine::start() -- no audio path implemented yet");
        running_ = true;
    }

    void stop() override { running_ = false; }

    bool is_running() const override { return running_; }

private:
    std::vector<config::StationConfig> stations_;
    bool running_ = false;
};

} // namespace

std::unique_ptr<AudioEngine> make_null_audio_engine(const config::StationRegistry& stations) {
    return std::make_unique<NullAudioEngine>(stations.all());
}

} // namespace icom::audio
