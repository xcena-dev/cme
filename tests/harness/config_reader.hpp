// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// config_reader.hpp -- config.yaml, read once and checked against the machine.
//
// Per-machine facts: which devdax node this host has, where the uncacheable mount is. None
// of it is true on another host, so none of it belongs in the build files or in a source
// default, and none of it comes from the environment: an environment variable travels
// invisibly, so a run behaves differently depending on who launched it and neither the
// ctest line nor the source says why.
//
// The file is restricted to flat `key: value`. That subset is valid YAML and is also what
// cmake/SiteConfig.cmake and the sweep scripts each parse on their own, so no consumer
// needs a parser dependency. Those two read keys this does not (the RoCE settings), which
// is why the file holds more than site() reports.
//
// Reading a key and trusting it are different things, so nothing here hands back a raw
// value. A declared devdax node that is not a character device, and a declared mount
// directory with nothing mounted on it, both read as absent -- which is what they are, to
// a run that wanted them.

#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

#ifndef CME_SITE_CONFIG_PATH
#define CME_SITE_CONFIG_PATH ""
#endif

namespace harness
{

// IEC byte units, so a size below reads as the size it is rather than as a shift.
inline constexpr std::uint64_t KiB = 1024;
inline constexpr std::uint64_t MiB = 1024 * KiB;
inline constexpr std::uint64_t GiB = 1024 * MiB;

class ConfigReader
{
public:
    // What this machine turned out to offer. Empty or zero means "not here", whether the
    // key was absent, left blank, or named something that is not what it claimed to be.
    struct Site_t
    {
        std::string daxDevice;       // only when it is a character device
        std::string fileBackendDir;  // only when something is mounted on it
        std::uint64_t daxSlotReserve;
        std::uint64_t daxSlotBase;  // 0 leaves the placement to the reserve
    };

    // A missing file is not an error: every fact then reads as absent, and a run that
    // wanted one of them fails when it asks, not here.
    explicit ConfigReader(std::string path = CME_SITE_CONFIG_PATH)
        : path_{std::move(path)}
    {
        parse();
        resolve();
    }

    [[nodiscard]] const Site_t& site() const noexcept
    {
        return site_;
    }

    // Named in the message when a run refuses, so the reader is told which file to fix.
    [[nodiscard]] const std::string& path() const noexcept
    {
        return path_;
    }

private:
    // Bytes held back at the end of a devdax node for the windows runs take, which is 512
    // of them at the stride TestMemory uses. They sit at the far end because a filesystem
    // sharing the device keeps its metadata at offset 0 and grows up from there.
    static constexpr std::uint64_t DefaultSlotReserve = 1 * GiB;

    void parse()
    {
        std::ifstream file{path_};
        if (!file)
        {
            return;
        }
        std::string line;
        while (std::getline(file, line))
        {
            const std::size_t hash = line.find('#');
            if (hash != std::string::npos)
            {
                line.resize(hash);
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos)
            {
                continue;  // blank, comment-only, or a line we do not model
            }
            const std::string key = trim(line.substr(0, colon));
            if (!key.empty())
            {
                kv_[key] = unquote(trim(line.substr(colon + 1)));
            }
        }
    }

    void resolve()
    {
        const std::string device = get("dax_device");
        if (!device.empty() && isCharDevice(device))
        {
            site_.daxDevice = device;
        }

        const std::string dir = get("file_backend_dir");
        if (!dir.empty() && isMountPoint(dir))
        {
            site_.fileBackendDir = dir;
        }

        site_.daxSlotReserve = getU64("dax_slot_reserve", DefaultSlotReserve);
        site_.daxSlotBase = getU64("dax_slot_base", 0);
    }

    [[nodiscard]] std::string get(const std::string& key) const
    {
        const auto iter = kv_.find(key);
        return (iter != kv_.end()) ? iter->second : std::string{};
    }

    [[nodiscard]] std::uint64_t getU64(const std::string& key, std::uint64_t fallback) const
    {
        const std::string raw = get(key);
        return raw.empty() ? fallback : std::strtoull(raw.c_str(), nullptr, 0);
    }

    // A devdax node is a character device. A regular file with the same path would take
    // every dax write to a filesystem instead.
    [[nodiscard]] static bool isCharDevice(const std::string& path)
    {
        struct stat info = {};
        return ::stat(path.c_str(), &info) == 0 && S_ISCHR(info.st_mode);
    }

    // True when @path is the root of a mounted filesystem, which is the case when its device
    // differs from its parent's. Deliberately does not ask which filesystem: the magic
    // number of the one we expect is not public, and "something is mounted here" is the
    // property that matters. The directory a mount used to occupy survives the unmount, so
    // without this a uc run would go to the root filesystem, treat page cache as an
    // uncacheable mapping, and report a code defect for a mount that is simply down.
    [[nodiscard]] static bool isMountPoint(const std::string& path)
    {
        struct stat self = {}, parent = {};
        if (::stat(path.c_str(), &self) != 0 || ::stat((path + "/..").c_str(), &parent) != 0)
        {
            return false;
        }
        return self.st_dev != parent.st_dev;
    }

    // Takes a view: both callers hand it a substring of the line, and the result is a fresh
    // string either way, so copying the input first buys nothing.
    static std::string trim(std::string_view text)
    {
        const char* blanks = " \t\r\n";
        const std::size_t begin = text.find_first_not_of(blanks);
        if (begin == std::string_view::npos)
        {
            return {};
        }
        return std::string{text.substr(begin, text.find_last_not_of(blanks) - begin + 1)};
    }

    static std::string unquote(std::string text)
    {
        if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') ||
                                 (text.front() == '\'' && text.back() == '\'')))
        {
            return text.substr(1, text.size() - 2);
        }
        return text;
    }

    std::map<std::string, std::string> kv_;
    std::string path_;
    Site_t site_{};
};

}  // namespace harness
