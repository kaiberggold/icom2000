// Covers icom::config::ConfigFile (parsing, get/get_uint/get_list, the
// missing-vs-malformed-file distinction) and StationRegistry (defaults
// when [stations]/[station.*] are absent, and picking up real values when
// present). See docs/ARCHITECTURE.md "Configuration" for why this exists.
#include "check.hpp"

#include "icom/config/config_file.hpp"
#include "icom/config/station_registry.hpp"

using namespace icom::config;

namespace {

void test_parse_reads_sections_and_keys() {
    const auto result = ConfigFile::parse(
        "[daemon]\n"
        "socket_path = /run/icom2000.sock\n"
        "# a comment, ignored\n"
        "\n"
        "[gpio.bell]\n"
        "line = 17\n");

    CHECK(result.ok);
    CHECK(result.config.get("daemon", "socket_path", "") == "/run/icom2000.sock");
    CHECK(result.config.get_uint("gpio.bell", "line", 0) == 17);
}

void test_parse_trims_whitespace_around_key_and_value() {
    const auto result = ConfigFile::parse("[a]\n   key   =   value with spaces   \n");
    CHECK(result.ok);
    CHECK(result.config.get("a", "key", "") == "value with spaces");
}

void test_parse_tolerates_crlf_line_endings() {
    const auto result = ConfigFile::parse("[a]\r\nkey = value\r\n");
    CHECK(result.ok);
    CHECK(result.config.get("a", "key", "") == "value");
}

void test_get_returns_default_when_absent() {
    const auto result = ConfigFile::parse("[a]\nkey = value\n");
    CHECK(result.ok);
    CHECK(result.config.get("a", "missing_key", "fallback") == "fallback");
    CHECK(result.config.get("missing_section", "key", "fallback") == "fallback");
}

void test_get_list_splits_and_trims_commas() {
    const auto result = ConfigFile::parse("[stations]\nnames = door,  inside ,  \n");
    CHECK(result.ok);
    const std::vector<std::string> names = result.config.get_list("stations", "names");
    CHECK(names.size() == 2 && names[0] == "door" && names[1] == "inside");
}

void test_get_list_empty_when_key_absent() {
    const auto result = ConfigFile::parse("[a]\nkey = value\n");
    CHECK(result.ok);
    CHECK(result.config.get_list("a", "no_such_key").empty());
}

void test_parse_rejects_key_outside_any_section() {
    const auto result = ConfigFile::parse("key = value\n");
    CHECK(!result.ok);
    CHECK(!result.error.empty());
}

void test_parse_rejects_malformed_section_header() {
    const auto result = ConfigFile::parse("[unterminated\n");
    CHECK(!result.ok);
}

void test_parse_rejects_line_without_equals_or_section() {
    const auto result = ConfigFile::parse("[a]\njust some words\n");
    CHECK(!result.ok);
}

void test_load_missing_file_is_not_an_error() {
    const auto result = ConfigFile::load("/nonexistent/path/that/should/not/exist/icom2000.conf");
    CHECK(result.ok);
    CHECK(!result.file_found);
    // Falls back cleanly to defaults through the normal get() API.
    CHECK(result.config.get("daemon", "socket_path", "default") == "default");
}

void test_station_registry_falls_back_to_door_and_inside_when_unconfigured() {
    const auto result = ConfigFile::parse("[daemon]\nsocket_path = /run/x.sock\n");
    CHECK(result.ok);

    const StationRegistry stations(result.config);
    CHECK(stations.all().size() == 2);

    const StationConfig* door = stations.find("door");
    CHECK(door != nullptr);
    CHECK(door != nullptr && door->capture_device == "icom_door_capture");
    CHECK(door != nullptr && door->playback_device == "icom_door_playback");

    const StationConfig* inside = stations.find("inside");
    CHECK(inside != nullptr);
    CHECK(inside != nullptr && inside->capture_device == "icom_inside_capture");

    CHECK(stations.find("no_such_station") == nullptr);
}

void test_station_registry_honors_explicit_config() {
    const auto result = ConfigFile::parse(
        "[stations]\n"
        "names = door\n"
        "\n"
        "[station.door]\n"
        "capture_device = custom_capture\n"
        "playback_device = custom_playback\n");
    CHECK(result.ok);

    const StationRegistry stations(result.config);
    CHECK(stations.all().size() == 1);

    const StationConfig* door = stations.find("door");
    CHECK(door != nullptr);
    CHECK(door != nullptr && door->capture_device == "custom_capture");
    CHECK(door != nullptr && door->playback_device == "custom_playback");
    CHECK(stations.find("inside") == nullptr);
}

} // namespace

int main() {
    test_parse_reads_sections_and_keys();
    test_parse_trims_whitespace_around_key_and_value();
    test_parse_tolerates_crlf_line_endings();
    test_get_returns_default_when_absent();
    test_get_list_splits_and_trims_commas();
    test_get_list_empty_when_key_absent();
    test_parse_rejects_key_outside_any_section();
    test_parse_rejects_malformed_section_header();
    test_parse_rejects_line_without_equals_or_section();
    test_load_missing_file_is_not_an_error();
    test_station_registry_falls_back_to_door_and_inside_when_unconfigured();
    test_station_registry_honors_explicit_config();

    const int failures = icom::testing::failure_count();
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
