#pragma once

#include "icom/config/config_file.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace icom::config {

// The one place a station name is tied to a physical/logical audio
// channel -- see docs/ARCHITECTURE.md "Stations". Everything else in the
// codebase works only with station names; nothing outside this type (and
// the config file it's built from) should ever need to know how "door" or
// "inside" map onto the codec's channels.
//
// `capture_device`/`playback_device` are named ALSA PCM devices, defined
// in config/asound.conf (installed as /etc/asound.conf) -- never a raw
// sound-card device string. Not consumed by anything yet (AudioEngine is
// still a stub, see docs/ARCHITECTURE.md "Audio boundary"), but every
// station config already carries these names so a future real
// implementation has a place to get a device name from other than
// inventing one inline.
struct StationConfig {
    std::string name;
    std::string capture_device;
    std::string playback_device;
};

class StationRegistry {
public:
    // Reads [stations] names=... and one [station.<name>] section per
    // listed name. Missing pieces fall back to defaults rather than
    // failing: an absent [stations] section falls back to the shipped
    // config/icom2000.conf's station list ("door", "inside"), and a
    // station section missing capture_device/playback_device falls back
    // to "icom_<name>_capture"/"icom_<name>_playback" -- so behavior is
    // identical whether or not a config file happens to be installed.
    explicit StationRegistry(const ConfigFile& config);

    const StationConfig* find(std::string_view name) const;
    const std::vector<StationConfig>& all() const { return stations_; }

private:
    std::vector<StationConfig> stations_;
};

} // namespace icom::config
