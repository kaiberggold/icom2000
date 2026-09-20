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
// `captureDevice`/`playbackDevice` are named ALSA PCM devices, defined
// in config/asound.conf (installed as /etc/asound.conf) -- never a raw
// sound-card device string. Not consumed by anything yet (Engine is
// still a stub, see docs/ARCHITECTURE.md "Audio boundary"), but every
// station config already carries these names so a future real
// implementation has a place to get a device name from other than
// inventing one inline.
struct Station {
    std::string name;
    std::string captureDevice;
    std::string playbackDevice;
};

class StationRegistry {
public:
    // Reads [stations] names=... and one [station.<name>] section per
    // listed name. Missing pieces fall back to defaults rather than
    // failing: an absent [stations] section falls back to the shipped
    // config/icom2000.conf's station list ("door", "inside"), and a
    // station section missing captureDevice/playbackDevice falls back
    // to "icom_<name>_capture"/"icom_<name>_playback" -- so behavior is
    // identical whether or not a config file happens to be installed.
    explicit StationRegistry(const File& config);

    const Station* find(std::string_view name) const;
    const std::vector<Station>& all() const { return stations_; }

private:
    std::vector<Station> stations_;
};

} // namespace icom::config
