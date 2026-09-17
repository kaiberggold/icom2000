#include "icom/audio/audio_engine.hpp"
#include "icom/core/logger.hpp"

namespace icom::audio {

namespace {

class NullAudioEngine final : public AudioEngine {
public:
    void start() override {
        core::log_warn("audio: NullAudioEngine::start() -- no audio path implemented yet");
        running_ = true;
    }

    void stop() override { running_ = false; }

    bool is_running() const override { return running_; }

private:
    bool running_ = false;
};

} // namespace

std::unique_ptr<AudioEngine> make_null_audio_engine() { return std::make_unique<NullAudioEngine>(); }

} // namespace icom::audio
