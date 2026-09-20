#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace icom::config {

// Minimal hand-rolled INI-style reader: "[section]" headers, "key = value"
// lines, "#"/";" full-line comments, blank lines ignored. No nesting, no
// quoting, no multi-line values, no include directives. This project's
// configuration is small and flat by design (see
// docs/ARCHITECTURE.md "Configuration") -- reach for a real parser/library
// only once that stops being true, rather than anticipating it here.
//
// Deliberately returns results rather than throwing across the load/parse
// boundary, matching icom::core::configure_levels()'s style: this reads
// content a human edits by hand (a systemd Environment=, a config file on
// the target), so a mistake there should become a clean, readable startup
// error, not an unhandled C++ exception.
class ConfigFile {
public:
    ConfigFile() = default;

    // Defined below, after this class closes: both hold a ConfigFile by
    // value, which needs this class to already be a complete type.
    struct ParseResult;
    struct LoadResult;

    static ParseResult parse(std::string_view text);

    // A missing file is reported via `file_found = false` with `ok = true`
    // -- callers typically fall back to built-in defaults in that case,
    // which is why it isn't folded into the `ok` failure path.
    static LoadResult load(const std::string& path);

    std::string get(std::string_view section, std::string_view key, std::string default_value) const;

    // Throws std::invalid_argument/std::out_of_range (via std::stoul) if
    // the value exists but isn't a valid unsigned integer -- a config
    // typo here should stop the daemon at startup, not silently fall back
    // to a default that no longer matches the file sitting right there.
    unsigned get_uint(std::string_view section, std::string_view key, unsigned default_value) const;

    // Splits on commas, trims whitespace around each entry, drops empty
    // entries. Returns an empty vector if the key is absent.
    std::vector<std::string> get_list(std::string_view section, std::string_view key) const;

    bool has_section(std::string_view section) const;

private:
    using Section = std::map<std::string, std::string, std::less<>>;

    std::map<std::string, Section, std::less<>> sections_;
};

struct ConfigFile::ParseResult {
    ConfigFile config;
    bool ok = true;
    std::string error; // set (with a 1-based line number) iff !ok
};

struct ConfigFile::LoadResult {
    ConfigFile config;   // always valid to read from, even on a missing file
    bool file_found = false;
    bool ok = true;       // false only if the file existed but failed to parse
    std::string error;    // set iff !ok
};

} // namespace icom::config
