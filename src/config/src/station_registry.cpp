#include "icom/config/station_registry.hpp"

#include <algorithm>

namespace icom::config {

StationRegistry::StationRegistry(const File& config) {
    std::vector<std::string> names = config.getList("stations", "names");
    if (names.empty()) {
        names = {"door", "inside"};
    }

    for (const auto& name : names) {
        const std::string section = "station." + name;
        Station station;
        station.name = name;
        station.captureDevice = config.get(section, "capture_device", "icom_" + name + "_capture");
        station.playbackDevice = config.get(section, "playback_device", "icom_" + name + "_playback");
        stations_.push_back(std::move(station));
    }
}

const Station* StationRegistry::find(std::string_view name) const {
    const auto it = std::find_if(stations_.begin(), stations_.end(),
    [&](const Station & s) { return s.name == name; });
    return it == stations_.end() ? nullptr : &*it;
}

} // namespace icom::config
