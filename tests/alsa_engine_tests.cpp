// Covers the ALSA engine (makeAlsaEngine, makeAlsaIntercomEngine) with no
// sound card: main() points HOME at a scratch directory whose .asoundrc
// defines test PCMs built on alsa-lib's own `file` plugin over its `null`
// device, before anything loads ALSA's config.
//
// Capture sides read their samples from FIFOs rather than regular files,
// deliberately: the `null` device isn't paced in real time, so over a
// regular file the engine would spin at hundreds of MB/s. Over a FIFO it
// consumes exactly the pattern the test wrote and then blocks in read() --
// the test's output is bounded and deterministic, with no timing guesswork.
#include "check.hpp"

#include "icom/audio/audio_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace icom;
using namespace std::chrono_literals;

namespace
{

    std::filesystem::path scratchDir;

    std::filesystem::path fifoPath(char station) { return scratchDir / (std::string("capture_") + station + ".fifo"); }
    std::filesystem::path playbackPath(char station) { return scratchDir / (std::string("playback_") + station + ".raw"); }

    // Test PCMs for two stations, "a" and "b": icom_test_capture_<x> reads
    // FIFO capture_<x>.fifo, icom_test_playback_<x> writes playback_<x>.raw.
    void writeAlsaConfig()
    {
        const std::filesystem::path home = scratchDir / "home";
        std::filesystem::create_directory(home);
        std::ofstream asoundrc(home / ".asoundrc");
        for (const char station : {'a', 'b'})
        {
            asoundrc << "pcm.icom_test_capture_" << station << " {\n"
                     << "    type file\n"
                     << "    slave.pcm null\n"
                     << "    file \"/dev/null\"\n"
                     << "    infile \"" << fifoPath(station).string() << "\"\n"
                     << "    format raw\n"
                     << "}\n"
                     << "pcm.icom_test_playback_" << station << " {\n"
                     << "    type file\n"
                     << "    slave.pcm null\n"
                     << "    file \"" << playbackPath(station).string() << "\"\n"
                     << "    format raw\n"
                     << "}\n";
        }
        ::setenv("HOME", home.c_str(), 1);
        ::unsetenv("XDG_CONFIG_HOME");
    }

    // Nonzero everywhere, so it can't be confused with the engine's leading
    // silence; not a multiple of any plausible period, so a dropped or
    // repeated period shows up as a mismatch.
    std::vector<std::int16_t> makePattern(std::int16_t base, int modulus)
    {
        std::vector<std::int16_t> pattern(4800);
        for (std::size_t i = 0; i < pattern.size(); ++i)
        {
            pattern[i] = static_cast<std::int16_t>(base + static_cast<int>(i) % modulus);
        }
        return pattern;
    }

    // A capture FIFO holding `pattern`. Opened O_RDWR, not O_WRONLY: opening
    // a FIFO write-only blocks until a reader shows up, and the reader is
    // the engine's start() -- on this same thread.
    class FeedFifo
    {
    public:
        FeedFifo(char station, const std::vector<std::int16_t>& pattern)
        {
            CHECK(::mkfifo(fifoPath(station).c_str(), 0600) == 0);
            fd_ = ::open(fifoPath(station).c_str(), O_RDWR);
            CHECK(fd_ >= 0);
            const auto bytes = static_cast<ssize_t>(pattern.size() * sizeof(std::int16_t));
            CHECK(::write(fd_, pattern.data(), static_cast<std::size_t>(bytes)) == bytes);
        }

        FeedFifo(const FeedFifo&) = delete;
        FeedFifo& operator=(const FeedFifo&) = delete;

        bool waitUntilDrained() const
        {
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            int unread = 1;
            while (unread > 0 && std::chrono::steady_clock::now() < deadline)
            {
                ::ioctl(fd_, FIONREAD, &unread);
                std::this_thread::sleep_for(1ms);
            }
            return unread == 0;
        }

        int fd() const { return fd_; }

    private:
        int fd_ = -1;
    };

    // Once drained, the engine is blocked reading the empty FIFO, which only
    // returns once the FIFO's last writer closes -- so close it just after
    // stop() has asked the audio thread(s) to finish, not before (from EOF
    // on, the `file` plugin's capture is unpaced again).
    void stopClosingFifos(audio::IEngine& engine, std::vector<int> fds)
    {
        std::jthread closer([fds]
        {
            std::this_thread::sleep_for(50ms);
            for (const int fd : fds)
            {
                ::close(fd);
            }
        });
        engine.stop();
    }

    // The playback file must hold only leading silence, then exactly
    // `pattern`. Anything after the pattern is the one read that returned
    // after the FIFO's EOF: harness noise, not engine output.
    bool playedExactly(const std::filesystem::path& path, const std::vector<std::int16_t>& pattern)
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<std::int16_t> played(std::filesystem::file_size(path) / sizeof(std::int16_t));
        in.read(reinterpret_cast<char*>(played.data()),
                static_cast<std::streamsize>(played.size() * sizeof(std::int16_t)));

        std::size_t first = 0;
        while (first < played.size() && played[first] == 0)
        {
            ++first;
        }
        return played.size() >= first + pattern.size() &&
               std::equal(pattern.begin(), pattern.end(), played.begin() + static_cast<std::ptrdiff_t>(first));
    }

    void testUnknownCaptureDeviceLeavesEngineStopped()
    {
        auto engine = audio::makeAlsaEngine(config::Station{"a", "icom_test_no_such_pcm", "unused"},
                                            config::Station{"b", "unused", "icom_test_playback_b"});
        engine->start();
        CHECK(!engine->isRunning());
        engine->stop(); // harmless on an engine that never ran
        CHECK(!engine->isRunning());
    }

    void testUnknownPlaybackDeviceLeavesEngineStopped()
    {
        auto engine = audio::makeAlsaEngine(config::Station{"a", "null", "unused"},
                                            config::Station{"b", "unused", "icom_test_no_such_pcm"});
        engine->start();
        CHECK(!engine->isRunning());
    }

    void testRoutesCapturedSamplesToPlaybackUnchanged()
    {
        const auto pattern = makePattern(1, 997);
        const FeedFifo feed('a', pattern);

        auto engine = audio::makeAlsaEngine(config::Station{"a", "icom_test_capture_a", "unused"},
                                            config::Station{"b", "unused", "icom_test_playback_b"});
        engine->start();
        CHECK(engine->isRunning());
        CHECK(feed.waitUntilDrained());

        stopClosingFifos(*engine, {feed.fd()});
        CHECK(!engine->isRunning());
        CHECK(playedExactly(playbackPath('b'), pattern));

        std::filesystem::remove(fifoPath('a'));
        std::filesystem::remove(playbackPath('b'));
    }

    void testIntercomCarriesBothDirectionsAtOnce()
    {
        const auto patternA = makePattern(1, 997);
        const auto patternB = makePattern(2000, 991);
        const FeedFifo feedA('a', patternA);
        const FeedFifo feedB('b', patternB);

        const config::Station a{"a", "icom_test_capture_a", "icom_test_playback_a"};
        const config::Station b{"b", "icom_test_capture_b", "icom_test_playback_b"};
        auto engine = audio::makeAlsaIntercomEngine(a, b);
        engine->start();
        CHECK(engine->isRunning());
        CHECK(feedA.waitUntilDrained());
        CHECK(feedB.waitUntilDrained());

        stopClosingFifos(*engine, {feedA.fd(), feedB.fd()});
        CHECK(!engine->isRunning());

        // Each station hears exactly the other's mic -- never its own.
        CHECK(playedExactly(playbackPath('b'), patternA));
        CHECK(playedExactly(playbackPath('a'), patternB));
    }

    std::size_t threadCount()
    {
        const std::filesystem::directory_iterator tasks("/proc/self/task");
        return static_cast<std::size_t>(std::distance(begin(tasks), end(tasks)));
    }

    void testIntercomStartsAllOrNothing()
    {
        // The a->b route could start; b->a can't. The group must not be
        // left running half an intercom -- including no leftover audio
        // thread from the route that did start.
        const std::size_t threadsBefore = threadCount();
        auto engine = audio::makeAlsaIntercomEngine(config::Station{"a", "null", "null"},
                      config::Station{"b", "icom_test_no_such_pcm", "null"});
        engine->start();
        CHECK(!engine->isRunning());
        CHECK(threadCount() == threadsBefore);
    }

} // namespace

int main()
{
    char dirTemplate[] = "/tmp/icom_alsa_test_XXXXXX";
    if (::mkdtemp(dirTemplate) == nullptr)
    {
        std::cerr << "mkdtemp failed\n";
        return 1;
    }
    scratchDir = dirTemplate;
    writeAlsaConfig();

    testUnknownCaptureDeviceLeavesEngineStopped();
    testUnknownPlaybackDeviceLeavesEngineStopped();
    testRoutesCapturedSamplesToPlaybackUnchanged();
    testIntercomCarriesBothDirectionsAtOnce();
    testIntercomStartsAllOrNothing();

    std::filesystem::remove_all(scratchDir);

    const int failures = icom::testing::failureCount();
    if (failures > 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
