#include "icom/config/station_registry.hpp"

#include <algorithm>

namespace icom::config {

StationRegistry::StationRegistry(const ConfigFile& config) {
    std::vector<std::string> names = config.get_list("stations", "names");
    if (names.empty()) {
        names = {"door", "inside"};
    }

    for (const auto& name : names) {
        const std::string section = "station." + name;
        StationConfig station;
        station.name = name;
        station.capture_device = config.get(section, "capture_device", "icom_" + name + "_capture");
        station.playback_device = config.get(section, "playback_device", "icom_" + name + "_playback");
        stations_.push_back(std::move(station));
    }
}

const StationConfig* StationRegistry::find(std::string_view name) const {
    const auto it = std::find_if(stations_.begin(), stations_.end(),
                                  [&](const StationConfig& s) { return s.name == name; });
    return it == stations_.end() ? nullptr : &*it;
}

} // namespace icom::config
