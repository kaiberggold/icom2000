#include "icom/config/config_file.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace icom::config {

namespace {

std::string_view trim(std::string_view s) {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string makeError(std::size_t lineNumber, std::string_view message) {
    return "line " + std::to_string(lineNumber) + ": " + std::string(message);
}

} // namespace

File::ParseResult File::parse(std::string_view text) {
    ParseResult result;
    std::string currentSection;
    std::size_t lineNumber = 0;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        ++lineNumber;
        std::size_t newline = text.find('\n', pos);
        std::string_view rawLine =
            text.substr(pos, newline == std::string_view::npos ? std::string_view::npos : newline - pos);
        pos = (newline == std::string_view::npos) ? text.size() + 1 : newline + 1;

        // Tolerate CRLF line endings.
        if (!rawLine.empty() && rawLine.back() == '\r') {
            rawLine.remove_suffix(1);
        }

        const std::string_view line = trim(rawLine);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        if (line.front() == '[') {
            if (line.back() != ']' || line.size() < 2) {
                result.ok = false;
                result.error = makeError(lineNumber, "malformed section header (expected \"[name]\")");
                return result;
            }
            currentSection = std::string(trim(line.substr(1, line.size() - 2)));
            if (currentSection.empty()) {
                result.ok = false;
                result.error = makeError(lineNumber, "empty section name");
                return result;
            }
            result.config.sections_.try_emplace(currentSection);
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            result.ok = false;
            result.error = makeError(lineNumber, "expected \"key = value\" or \"[section]\"");
            return result;
        }

        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        if (key.empty()) {
            result.ok = false;
            result.error = makeError(lineNumber, "empty key");
            return result;
        }
        if (currentSection.empty()) {
            result.ok = false;
            result.error = makeError(lineNumber, "key outside of any [section]");
            return result;
        }

        result.config.sections_[currentSection][std::string(key)] = std::string(value);
    }

    return result;
}

File::LoadResult File::load(const std::string& path) {
    LoadResult result;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.fileFound = false;
        return result;
    }
    result.fileFound = true;

    std::ostringstream buffer;
    buffer << file.rdbuf();

    ParseResult parsed = parse(buffer.str());
    result.ok = parsed.ok;
    result.error = std::move(parsed.error);
    result.config = std::move(parsed.config);
    return result;
}

std::string File::get(std::string_view section, std::string_view key, std::string defaultValue) const {
    const auto sectionIt = sections_.find(section);
    if (sectionIt == sections_.end()) {
        return defaultValue;
    }
    const auto keyIt = sectionIt->second.find(key);
    if (keyIt == sectionIt->second.end()) {
        return defaultValue;
    }
    return keyIt->second;
}

unsigned File::getUint(std::string_view section, std::string_view key, unsigned defaultValue) const {
    const std::string value = get(section, key, "");
    if (value.empty()) {
        return defaultValue;
    }
    return static_cast<unsigned>(std::stoul(value));
}

std::vector<std::string> File::getList(std::string_view section, std::string_view key) const {
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

bool File::hasSection(std::string_view section) const { return sections_.contains(section); }

} // namespace icom::config
