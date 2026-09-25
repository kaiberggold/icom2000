// Covers the ALSA engine (makeAlsaEngine) with no sound card: main() points
// HOME at a scratch directory whose .asoundrc defines test PCMs built on
// alsa-lib's own `file` plugin over its `null` device, before anything
// loads ALSA's config.
//
// The capture side reads its samples from a FIFO rather than a regular
// file, deliberately: the `null` device isn't paced in real time, so over a
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

    std::filesystem::path fifoPath() { return scratchDir / "capture.fifo"; }
    std::filesystem::path playbackPath() { return scratchDir / "playback.raw"; }

    void writeAlsaConfig()
    {
        const std::filesystem::path home = scratchDir / "home";
        std::filesystem::create_directory(home);
        std::ofstream(home / ".asoundrc")
                << "pcm.icom_test_fifo_capture {\n"
                << "    type file\n"
                << "    slave.pcm null\n"
                << "    file \"/dev/null\"\n"
                << "    infile \"" << fifoPath().string() << "\"\n"
                << "    format raw\n"
                << "}\n"
                << "pcm.icom_test_file_playback {\n"
                << "    type file\n"
                << "    slave.pcm null\n"
                << "    file \"" << playbackPath().string() << "\"\n"
                << "    format raw\n"
                << "}\n";
        ::setenv("HOME", home.c_str(), 1);
        ::unsetenv("XDG_CONFIG_HOME");
    }

    std::vector<std::int16_t> readSamples(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<std::int16_t> samples(std::filesystem::file_size(path) / sizeof(std::int16_t));
        in.read(reinterpret_cast<char*>(samples.data()),
                static_cast<std::streamsize>(samples.size() * sizeof(std::int16_t)));
        return samples;
    }

    void testUnknownCaptureDeviceLeavesEngineStopped()
    {
        auto engine = audio::makeAlsaEngine(config::Station{"a", "icom_test_no_such_pcm", "unused"},
                                            config::Station{"b", "unused", "icom_test_file_playback"});
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
        CHECK(::mkfifo(fifoPath().c_str(), 0600) == 0);
        // O_RDWR, not O_WRONLY: opening a FIFO write-only blocks until a
        // reader shows up, and the reader is the engine's start() -- on this
        // same thread.
        const int fifo = ::open(fifoPath().c_str(), O_RDWR);
        CHECK(fifo >= 0);

        // Nonzero everywhere, so it can't be confused with the engine's
        // leading silence; not a multiple of any plausible period, so a
        // dropped or repeated period shows up as a mismatch.
        std::vector<std::int16_t> pattern(4800);
        for (std::size_t i = 0; i < pattern.size(); ++i)
        {
            pattern[i] = static_cast<std::int16_t>(1 + i % 997);
        }
        const auto bytes = static_cast<ssize_t>(pattern.size() * sizeof(std::int16_t));
        CHECK(::write(fifo, pattern.data(), static_cast<std::size_t>(bytes)) == bytes);

        auto engine = audio::makeAlsaEngine(config::Station{"a", "icom_test_fifo_capture", "unused"},
                                            config::Station{"b", "unused", "icom_test_file_playback"});
        engine->start();
        CHECK(engine->isRunning());

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        int unread = 1;
        while (unread > 0 && std::chrono::steady_clock::now() < deadline)
        {
            ::ioctl(fifo, FIONREAD, &unread);
            std::this_thread::sleep_for(1ms);
        }
        CHECK(unread == 0);

        // The engine is now blocked reading the empty FIFO, which only
        // returns once the FIFO's last writer closes -- so close it just
        // after stop() has asked the audio thread to finish, not before
        // (from EOF on, the `file` plugin's capture is unpaced again).
        std::jthread closer([fifo]
        {
            std::this_thread::sleep_for(50ms);
            ::close(fifo);
        });
        engine->stop();
        CHECK(!engine->isRunning());
        closer.join();

        const std::vector<std::int16_t> played = readSamples(playbackPath());
        std::size_t first = 0;
        while (first < played.size() && played[first] == 0)
        {
            ++first;
        }
        CHECK(played.size() >= first + pattern.size());
        if (played.size() >= first + pattern.size())
        {
            // Anything after the pattern is the one read that returned
            // after the FIFO's EOF: harness noise, not engine output.
            CHECK(std::equal(pattern.begin(), pattern.end(),
                             played.begin() + static_cast<std::ptrdiff_t>(first)));
        }
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
