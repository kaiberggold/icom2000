// icom-audiotest: runs the ALSA intercom engine between the first two
// stations in the config -- each one's mic to the other's speaker -- until
// Ctrl-C or --seconds. For checking the audio path on real hardware before
// intercomd itself runs the engine (docs/SMOKE_TESTS.md "Step 2").
#include "icom/audio/audio_engine.hpp"
#include "icom/config/config_file.hpp"
#include "icom/config/station_registry.hpp"
#include "icom/core/logger.hpp"

#include <chrono>
#include <csignal>
#include <ctime>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include <pthread.h>

namespace
{

    void printUsage(const char* argv0)
    {
        std::cout << "usage: " << argv0 << " [--config PATH] [--seconds N]\n"
                  << "  Runs the intercom between the first two stations in the config, each\n"
                  << "  station's mic to the other's speaker, until Ctrl-C (or N seconds).\n"
                  << "  --config PATH  default /etc/icom2000.conf ([stations], [station.*], [audio])\n"
                  << "  Log levels: ICOM_LOG, as for intercomd (e.g. ICOM_LOG=debug).\n";
    }

} // namespace

int main(int argc, char** argv)
{
    std::string configPath = "/etc/icom2000.conf";
    unsigned seconds = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--config" && i + 1 < argc)
        {
            configPath = argv[++i];
        }
        else if (arg == "--seconds" && i + 1 < argc)
        {
            try
            {
                seconds = static_cast<unsigned>(std::stoul(argv[++i]));
            }
            catch (const std::exception&)
            {
                std::cerr << "icom-audiotest: --seconds needs a number\n";
                return 2;
            }
        }
        else
        {
            printUsage(argv[0]);
            return arg == "--help" ? 0 : 2;
        }
    }

    icom::core::setConsoleOutput(true);
    icom::core::configureLevels("info");
    if (!icom::core::configureLevelsFromEnv())
    {
        std::cerr << "icom-audiotest: invalid ICOM_LOG\n";
        return 2;
    }

    const auto configResult = icom::config::File::load(configPath);
    if (!configResult.ok)
    {
        std::cerr << "icom-audiotest: " << configPath << ": " << configResult.error << "\n";
        return 1;
    }
    if (!configResult.fileFound)
    {
        std::cerr << "icom-audiotest: no config file at " << configPath << " -- using built-in defaults\n";
    }
    const icom::config::File& config = configResult.config;

    const icom::config::StationRegistry stations(config);
    if (stations.all().size() < 2)
    {
        std::cerr << "icom-audiotest: needs at least two stations in [stations]\n";
        return 1;
    }
    const icom::config::Station& a = stations.all()[0];
    const icom::config::Station& b = stations.all()[1];

    unsigned rtPriority = 0;
    try
    {
        rtPriority = config.getUint("audio", "rt_priority", 20);
    }
    catch (const std::exception& e)
    {
        std::cerr << "icom-audiotest: " << configPath << ": invalid [audio] rt_priority (" << e.what() << ")\n";
        return 1;
    }

    // Blocked before any audio thread exists, so those threads inherit the
    // mask and Ctrl-C reaches the sigtimedwait() below instead.
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr);

    std::cout << a.name << " mic (" << a.captureDevice << ") -> " << b.name << " speaker (" << b.playbackDevice
              << ")\n"
              << b.name << " mic (" << b.captureDevice << ") -> " << a.name << " speaker (" << a.playbackDevice
              << ")\n";

    auto engine = icom::audio::makeAlsaIntercomEngine(a, b, static_cast<int>(rtPriority));
    engine->start();
    if (!engine->isRunning())
    {
        std::cerr << "icom-audiotest: audio failed to start -- see the errors above\n";
        return 1;
    }
    std::cout << "running -- Ctrl-C to stop\n";

    const auto started = std::chrono::steady_clock::now();
    int exitCode = 0;
    while (true)
    {
        const timespec oneSecond{1, 0};
        if (sigtimedwait(&stopSignals, nullptr, &oneSecond) > 0)
        {
            break;
        }
        if (!engine->isRunning())
        {
            std::cerr << "icom-audiotest: a route stopped on its own -- see the errors above\n";
            exitCode = 1;
            break;
        }
        if (seconds > 0 && std::chrono::steady_clock::now() - started >= std::chrono::seconds(seconds))
        {
            break;
        }
    }

    engine->stop();
    std::cout << "stopped\n";
    return exitCode;
}
