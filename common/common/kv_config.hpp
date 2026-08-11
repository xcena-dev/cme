// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// common/kv_config.hpp -- the YAML subset this repository reads.
//
// Shared by the test harness, which reads per-machine facts, and by cmed, which reads its daemon
// settings. Header-only and in its own namespace, so including it couples neither side to the
// other's library.
//
// THE SUBSET. Nested maps by indentation, `key: value` scalars, and inline `[a, b]` sequences. A
// nested key is addressed by its path: `region.uri`. That path, and not the leaf name, is what
// keeps `area.path` and `log.path` apart.
//
// Anything outside the subset is refused with a line number rather than skipped. A skipped line is
// a setting that reads as its default while the file plainly says otherwise, and nothing in a run
// afterwards points back at the typo.

#pragma once

#include <cstdint>
#include <fstream>
#include <istream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kvconfig
{

// Carries the file and the line, because the whole reason to refuse rather than skip is to say
// where to look.
class ParseError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class KeyValueConfig
{
public:
    // ── factories ──────────────────────────────────────────────────
    // Throws ParseError when the file cannot be opened. Use loadIfPresent where absence is a
    // normal answer rather than a fault.
    [[nodiscard]] static KeyValueConfig load(const std::string& path);

    // A missing file leaves every key absent, so each getter answers its fallback.
    [[nodiscard]] static KeyValueConfig loadIfPresent(const std::string& path);

    [[nodiscard]] static KeyValueConfig parse(std::istream& input, std::string origin);

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] bool has(const std::string& key) const
    {
        return entries_.count(key) != 0;
    }

    [[nodiscard]] std::string getString(const std::string& key, const std::string& fallback = {}) const
    {
        const auto found = entries_.find(key);
        if (found == entries_.end())
        {
            return fallback;
        }
        return found->second;
    }

    // Refuses a value that is not a number rather than reading as zero, which would be
    // indistinguishable from a deliberate 0 in the file.
    [[nodiscard]] std::uint64_t getU64(const std::string& key, std::uint64_t fallback) const;

    [[nodiscard]] bool getBool(const std::string& key, bool fallback) const;

    // Empty when the key is absent. `[]` and a key with no value both give an empty list.
    [[nodiscard]] std::vector<std::string> getList(const std::string& key) const;

    // Named in a refusal so the reader is told which file to fix.
    [[nodiscard]] const std::string& origin() const noexcept
    {
        return origin_;
    }

private:
    // One frame per open map: the column its children must exceed, and the prefix they carry.
    struct Section_t
    {
        std::uint64_t column;
        std::string prefix;
    };

    [[nodiscard]] std::string where(std::uint64_t line) const
    {
        return origin_ + ":" + std::to_string(line) + ": ";
    }

    [[nodiscard]] const std::string& require(const std::string& key) const;

    static std::string_view trim(std::string_view text);
    static std::string unquote(std::string_view text);
    static std::uint64_t indentOf(std::string_view line, std::uint64_t lineNumber, const std::string& origin);
    static std::string_view stripComment(std::string_view line);

    std::map<std::string, std::string> entries_;
    std::string origin_;
};

// ── parsing ────────────────────────────────────────────────────────────

inline std::string_view KeyValueConfig::trim(std::string_view text)
{
    constexpr std::string_view Blanks = " \t\r\n";
    const std::uint64_t begin = text.find_first_not_of(Blanks);
    if (begin == std::string_view::npos)
    {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(Blanks) - begin + 1);
}

inline std::string KeyValueConfig::unquote(std::string_view text)
{
    const bool quoted = text.size() >= 2 && text.front() == text.back() &&
                        (text.front() == '"' || text.front() == '\'');
    return quoted ? std::string{text.substr(1, text.size() - 2)} : std::string{text};
}

// A '#' opens a comment only at the start of the line or after a blank, which is the YAML rule and
// what lets a value such as `shm:/area#0` keep its own hash. A quoted value is left whole.
inline std::string_view KeyValueConfig::stripComment(std::string_view line)
{
    char quote = '\0';
    for (std::uint64_t index = 0; index < line.size(); ++index)
    {
        const char letter = line[index];
        if (quote != '\0')
        {
            if (letter == quote)
            {
                quote = '\0';
            }
            continue;
        }
        if (letter == '"' || letter == '\'')
        {
            quote = letter;
            continue;
        }
        if (letter == '#' && (index == 0 || line[index - 1] == ' ' || line[index - 1] == '\t'))
        {
            return line.substr(0, index);
        }
    }
    return line;
}

// Tabs are refused rather than counted. YAML forbids them in indentation, and one tab counted as
// one column would place a child under the wrong parent without any line looking wrong.
inline std::uint64_t
KeyValueConfig::indentOf(std::string_view line, std::uint64_t lineNumber, const std::string& origin)
{
    const std::uint64_t column = line.find_first_not_of(' ');
    if (column != std::string_view::npos && line[column] == '\t')
    {
        throw ParseError{origin + ":" + std::to_string(lineNumber) + ": tab in indentation"};
    }
    return column == std::string_view::npos ? 0 : column;
}

inline KeyValueConfig KeyValueConfig::parse(std::istream& input, std::string origin)
{
    KeyValueConfig config;
    config.origin_ = std::move(origin);

    std::vector<Section_t> open;
    std::string line;
    std::uint64_t lineNumber = 0;

    while (std::getline(input, line))
    {
        ++lineNumber;
        const std::string_view body = stripComment(line);
        if (trim(body).empty())
        {
            continue;
        }

        const std::uint64_t column = indentOf(body, lineNumber, config.origin_);
        while (!open.empty() && open.back().column >= column)
        {
            open.pop_back();
        }

        const std::uint64_t colon = body.find(':');
        if (colon == std::string_view::npos)
        {
            throw ParseError{config.where(lineNumber) + "not `key: value`"};
        }

        const std::string_view name = trim(body.substr(0, colon));
        if (name.empty())
        {
            throw ParseError{config.where(lineNumber) + "empty key"};
        }

        const std::string path = (open.empty() ? std::string{} : open.back().prefix) + std::string{name};
        const std::string_view value = trim(body.substr(colon + 1));

        // Stored even when empty, and a frame opened either way. A key whose value is blank is a
        // section when deeper lines follow it and an empty setting when they do not, and which one
        // it is only becomes visible on the next line.
        config.entries_[path] = unquote(value);
        if (value.empty())
        {
            open.push_back(Section_t{column, path + "."});
        }
    }

    return config;
}

inline KeyValueConfig KeyValueConfig::load(const std::string& path)
{
    std::ifstream file{path};
    if (!file)
    {
        throw ParseError{path + ": cannot open"};
    }
    return parse(file, path);
}

inline KeyValueConfig KeyValueConfig::loadIfPresent(const std::string& path)
{
    std::ifstream file{path};
    if (!file)
    {
        KeyValueConfig empty;
        empty.origin_ = path;
        return empty;
    }
    return parse(file, path);
}

// ── typed reads ────────────────────────────────────────────────────────

inline const std::string& KeyValueConfig::require(const std::string& key) const
{
    return entries_.at(key);
}

inline std::uint64_t KeyValueConfig::getU64(const std::string& key, std::uint64_t fallback) const
{
    if (!has(key) || require(key).empty())
    {
        return fallback;
    }

    const std::string& raw = require(key);
    std::istringstream reader{raw};
    std::uint64_t value = 0;
    reader >> value;
    if (reader.fail() || !reader.eof())
    {
        throw ParseError{origin_ + ": " + key + ": `" + raw + "` is not a number"};
    }
    return value;
}

inline bool KeyValueConfig::getBool(const std::string& key, bool fallback) const
{
    if (!has(key) || require(key).empty())
    {
        return fallback;
    }

    const std::string& raw = require(key);
    if (raw == "true" || raw == "yes" || raw == "on" || raw == "1")
    {
        return true;
    }
    if (raw == "false" || raw == "no" || raw == "off" || raw == "0")
    {
        return false;
    }
    throw ParseError{origin_ + ": " + key + ": `" + raw + "` is not a boolean"};
}

inline std::vector<std::string> KeyValueConfig::getList(const std::string& key) const
{
    std::vector<std::string> items;
    if (!has(key))
    {
        return items;
    }

    std::string_view raw = require(key);
    if (raw.empty())
    {
        return items;
    }
    if (raw.front() != '[' || raw.back() != ']')
    {
        throw ParseError{origin_ + ": " + key + ": `" + std::string{raw} + "` is not an inline sequence"};
    }

    raw = raw.substr(1, raw.size() - 2);
    while (!trim(raw).empty())
    {
        const std::uint64_t comma = raw.find(',');
        items.push_back(unquote(trim(raw.substr(0, comma))));
        if (comma == std::string_view::npos)
        {
            break;
        }
        raw = raw.substr(comma + 1);
    }
    return items;
}

}  // namespace kvconfig
