#include "icom/config/config_file.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace icom::config {

namespace {

std::string_view trim(std::string_view s) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string make_error(std::size_t line_number, std::string_view message) {
    return "line " + std::to_string(line_number) + ": " + std::string(message);
}

} // namespace

ConfigFile::ParseResult ConfigFile::parse(std::string_view text) {
    ParseResult result;
    std::string current_section;
    std::size_t line_number = 0;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        ++line_number;
        std::size_t newline = text.find('\n', pos);
        std::string_view raw_line =
            text.substr(pos, newline == std::string_view::npos ? std::string_view::npos : newline - pos);
        pos = (newline == std::string_view::npos) ? text.size() + 1 : newline + 1;

        // Tolerate CRLF line endings.
        if (!raw_line.empty() && raw_line.back() == '\r') {
            raw_line.remove_suffix(1);
        }

        const std::string_view line = trim(raw_line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        if (line.front() == '[') {
            if (line.back() != ']' || line.size() < 2) {
                result.ok = false;
                result.error = make_error(line_number, "malformed section header (expected \"[name]\")");
                return result;
            }
            current_section = std::string(trim(line.substr(1, line.size() - 2)));
            if (current_section.empty()) {
                result.ok = false;
                result.error = make_error(line_number, "empty section name");
                return result;
            }
            result.config.sections_.try_emplace(current_section);
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            result.ok = false;
            result.error = make_error(line_number, "expected \"key = value\" or \"[section]\"");
            return result;
        }

        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        if (key.empty()) {
            result.ok = false;
            result.error = make_error(line_number, "empty key");
            return result;
        }
        if (current_section.empty()) {
            result.ok = false;
            result.error = make_error(line_number, "key outside of any [section]");
            return result;
        }

        result.config.sections_[current_section][std::string(key)] = std::string(value);
    }

    return result;
}

ConfigFile::LoadResult ConfigFile::load(const std::string& path) {
    LoadResult result;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.file_found = false;
        return result;
    }
    result.file_found = true;

    std::ostringstream buffer;
    buffer << file.rdbuf();

    ParseResult parsed = parse(buffer.str());
    result.ok = parsed.ok;
    result.error = std::move(parsed.error);
    result.config = std::move(parsed.config);
    return result;
}

std::string ConfigFile::get(std::string_view section, std::string_view key, std::string default_value) const {
    const auto section_it = sections_.find(section);
    if (section_it == sections_.end()) {
        return default_value;
    }
    const auto key_it = section_it->second.find(key);
    if (key_it == section_it->second.end()) {
        return default_value;
    }
    return key_it->second;
}

unsigned ConfigFile::get_uint(std::string_view section, std::string_view key, unsigned default_value) const {
    const std::string value = get(section, key, "");
    if (value.empty()) {
        return default_value;
    }
    return static_cast<unsigned>(std::stoul(value));
}

std::vector<std::string> ConfigFile::get_list(std::string_view section, std::string_view key) const {
    std::vector<std::string> items;
    const std::string value = get(section, key, "");

    std::size_t pos = 0;
    while (pos <= value.size()) {
        const std::size_t comma = value.find(',', pos);
        const std::string_view item = trim(std::string_view(value).substr(
            pos, comma == std::string::npos ? std::string_view::npos : comma - pos));
        pos = (comma == std::string::npos) ? value.size() + 1 : comma + 1;
        if (!item.empty()) {
            items.emplace_back(item);
        }
    }
    return items;
}

bool ConfigFile::has_section(std::string_view section) const { return sections_.contains(section); }

} // namespace icom::config
