#include "icom/audio/audio_engine.hpp"
#include "icom/core/logger.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>

namespace icom::audio
{

    namespace
    {

        core::Logger& log = core::getLogger("audio.alsa");

        constexpr unsigned SAMPLE_RATE = 48000;
        constexpr unsigned CHANNELS = 1;
        // Requested playback buffer size, which (with the prefill below) is
        // also roughly the route's end-to-end latency.
        constexpr unsigned LATENCY_US = 40000;
        // Bounds how long stop() can take if the capture device stops
        // delivering data entirely.
        constexpr int WAIT_TIMEOUT_MS = 100;

        std::string alsaError(long err)
        {
            return snd_strerror(static_cast<int>(err));
        }

        struct PcmCloser
        {
            void operator()(snd_pcm_t* pcm) const { snd_pcm_close(pcm); }
        };
        using PcmHandle = std::unique_ptr<snd_pcm_t, PcmCloser>;

        PcmHandle openPcm(const std::string& device, snd_pcm_stream_t stream)
        {
            const char* direction = stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback";
            snd_pcm_t* raw = nullptr;
            int err = snd_pcm_open(&raw, device.c_str(), stream, 0);
            if (err < 0)
            {
                log.error(std::string("cannot open ") + direction + " device " + device + ": " + alsaError(err));
                return nullptr;
            }
            PcmHandle pcm(raw);
            err = snd_pcm_set_params(pcm.get(), SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, CHANNELS,
                                     SAMPLE_RATE, /*soft_resample=*/1, LATENCY_US);
            if (err < 0)
            {
                log.error(std::string("cannot configure ") + direction + " device " + device + ": " +
                          alsaError(err));
                return nullptr;
            }
            return pcm;
        }

        class AlsaEngine final : public IEngine
        {
        public:
            AlsaEngine(std::string captureDevice, std::string playbackDevice, int rtPriority)
                : captureDevice_(std::move(captureDevice)), playbackDevice_(std::move(playbackDevice)),
                  rtPriority_(rtPriority) {}

            ~AlsaEngine() override { stop(); }

            AlsaEngine(const AlsaEngine&) = delete;
            AlsaEngine& operator=(const AlsaEngine&) = delete;

            void start() override
            {
                if (running_.load())
                {
                    return;
                }
                // Releases the devices of a previous run whose thread died on
                // an unrecoverable error, before reopening them.
                stop();

                PcmHandle capture = openPcm(captureDevice_, SND_PCM_STREAM_CAPTURE);
                if (!capture)
                {
                    return;
                }
                PcmHandle playback = openPcm(playbackDevice_, SND_PCM_STREAM_PLAYBACK);
                if (!playback)
                {
                    return;
                }

                snd_pcm_uframes_t captureBufferFrames = 0;
                snd_pcm_get_params(capture.get(), &captureBufferFrames, &capturePeriodFrames_);
                snd_pcm_get_params(playback.get(), &playbackBufferFrames_, &playbackPeriodFrames_);

                capture_ = std::move(capture);
                playback_ = std::move(playback);
                if (!prefillPlayback() || !startCapture())
                {
                    capture_.reset();
                    playback_.reset();
                    return;
                }

                log.info("routing " + captureDevice_ + " -> " + playbackDevice_ + ": " +
                         std::to_string(SAMPLE_RATE) + " Hz mono S16_LE, capture period " +
                         std::to_string(capturePeriodFrames_) + " frames, playback buffer " +
                         std::to_string(playbackBufferFrames_) + " frames");
                running_.store(true);
                thread_ = std::jthread([this](std::stop_token stopToken) { run(stopToken); });
            }

            void stop() override
            {
                if (thread_.joinable())
                {
                    thread_.request_stop();
                    thread_.join();
                }
                capture_.reset();
                playback_.reset();
                running_.store(false);
            }

            bool isRunning() const override { return running_.load(); }

        private:
            void run(const std::stop_token& stopToken)
            {
                raiseToRealtimePriority();
                std::vector<std::int16_t> buffer(capturePeriodFrames_ * CHANNELS);
                while (!stopToken.stop_requested())
                {
                    const int ready = snd_pcm_wait(capture_.get(), WAIT_TIMEOUT_MS);
                    if (ready == 0)
                    {
                        continue;
                    }
                    if (ready < 0)
                    {
                        if (!recoverCapture(ready))
                        {
                            break;
                        }
                        continue;
                    }

                    const snd_pcm_sframes_t got = snd_pcm_readi(capture_.get(), buffer.data(), capturePeriodFrames_);
                    if (got == -EAGAIN)
                    {
                        continue;
                    }
                    if (got < 0)
                    {
                        if (!recoverCapture(got))
                        {
                            break;
                        }
                        continue;
                    }
                    if (!writeToPlayback(buffer.data(), static_cast<snd_pcm_uframes_t>(got)))
                    {
                        break;
                    }
                }
                if (!stopToken.stop_requested())
                {
                    log.error("audio thread stopped after an unrecoverable error on " + captureDevice_ + " -> " +
                              playbackDevice_);
                }
                running_.store(false);
            }

            void raiseToRealtimePriority() const
            {
                if (rtPriority_ <= 0)
                {
                    return;
                }
                sched_param param{};
                param.sched_priority = rtPriority_;
                const int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
                const std::string route = captureDevice_ + " -> " + playbackDevice_;
                if (err != 0)
                {
                    log.warn("audio thread for " + route + " runs at normal priority, not SCHED_FIFO " +
                             std::to_string(rtPriority_) + ": " + std::strerror(err) +
                             " (needs CAP_SYS_NICE or RLIMIT_RTPRIO, see docs/ARCHITECTURE.md \"Audio boundary\")");
                    return;
                }
                log.debug("audio thread for " + route + " runs at SCHED_FIFO " + std::to_string(rtPriority_));
            }

            bool writeToPlayback(const std::int16_t* data, snd_pcm_uframes_t frames)
            {
                while (frames > 0)
                {
                    const snd_pcm_sframes_t written = snd_pcm_writei(playback_.get(), data, frames);
                    if (written == -EAGAIN)
                    {
                        continue;
                    }
                    if (written < 0)
                    {
                        if (!recoverPlayback(written))
                        {
                            return false;
                        }
                        continue;
                    }
                    data += static_cast<std::size_t>(written) * CHANNELS;
                    frames -= static_cast<snd_pcm_uframes_t>(written);
                }
                return true;
            }

            bool recoverCapture(long err)
            {
                if (snd_pcm_recover(capture_.get(), static_cast<int>(err), /*silent=*/1) < 0)
                {
                    log.error("capture on " + captureDevice_ + " failed: " + alsaError(err));
                    return false;
                }
                log.warn("capture on " + captureDevice_ + " recovered from: " + alsaError(err));
                return startCapture();
            }

            bool recoverPlayback(long err)
            {
                if (snd_pcm_recover(playback_.get(), static_cast<int>(err), /*silent=*/1) < 0)
                {
                    log.error("playback on " + playbackDevice_ + " failed: " + alsaError(err));
                    return false;
                }
                log.warn("playback on " + playbackDevice_ + " recovered from: " + alsaError(err));
                return prefillPlayback();
            }

            // Capture doesn't start on its own after being (re)prepared, and
            // snd_pcm_wait() on a stream that isn't running would only ever
            // time out.
            bool startCapture()
            {
                if (snd_pcm_state(capture_.get()) != SND_PCM_STATE_PREPARED)
                {
                    return true;
                }
                const int err = snd_pcm_start(capture_.get());
                if (err < 0)
                {
                    log.error("cannot start capture on " + captureDevice_ + ": " + alsaError(err));
                    return false;
                }
                return true;
            }

            // snd_pcm_set_params() makes playback start only once its buffer is
            // full, so priming it with one period short of that means the first
            // captured period starts it, leaving a buffer's worth of slack
            // before an underrun. Only when (re)prepared: writing silence into
            // a running stream would add latency permanently.
            bool prefillPlayback()
            {
                if (snd_pcm_state(playback_.get()) != SND_PCM_STATE_PREPARED)
                {
                    return true;
                }
                const snd_pcm_uframes_t frames = playbackBufferFrames_ - playbackPeriodFrames_;
                const std::vector<std::int16_t> silence(frames * CHANNELS, 0);
                const snd_pcm_sframes_t written = snd_pcm_writei(playback_.get(), silence.data(), frames);
                if (written < 0)
                {
                    log.error("cannot prime playback on " + playbackDevice_ + ": " + alsaError(written));
                    return false;
                }
                return true;
            }

            const std::string captureDevice_;
            const std::string playbackDevice_;
            const int rtPriority_;

            PcmHandle capture_;
            PcmHandle playback_;
            snd_pcm_uframes_t capturePeriodFrames_ = 0;
            snd_pcm_uframes_t playbackBufferFrames_ = 0;
            snd_pcm_uframes_t playbackPeriodFrames_ = 0;

            std::atomic<bool> running_ = false;
            std::jthread thread_;
        };

        class EngineGroup final : public IEngine
        {
        public:
            explicit EngineGroup(std::vector<std::unique_ptr<IEngine>> members) : members_(std::move(members)) {}

            void start() override
            {
                for (const auto& member : members_)
                {
                    member->start();
                    if (!member->isRunning())
                    {
                        stop();
                        return;
                    }
                }
            }

            void stop() override
            {
                for (const auto& member : members_)
                {
                    member->stop();
                }
            }

            bool isRunning() const override
            {
                return std::all_of(members_.begin(), members_.end(),
                [](const auto & member) { return member->isRunning(); });
            }

        private:
            std::vector<std::unique_ptr<IEngine>> members_;
        };

    } // namespace

    std::unique_ptr<IEngine> makeAlsaEngine(const config::Station& captureFrom, const config::Station& playbackTo,
                                            int rtPriority)
    {
        return std::make_unique<AlsaEngine>(captureFrom.captureDevice, playbackTo.playbackDevice, rtPriority);
    }

    std::unique_ptr<IEngine> makeAlsaIntercomEngine(const config::Station& a, const config::Station& b,
            int rtPriority)
    {
        std::vector<std::unique_ptr<IEngine>> routes;
        routes.push_back(makeAlsaEngine(a, b, rtPriority));
        routes.push_back(makeAlsaEngine(b, a, rtPriority));
        return std::make_unique<EngineGroup>(std::move(routes));
    }

} // namespace icom::audio
