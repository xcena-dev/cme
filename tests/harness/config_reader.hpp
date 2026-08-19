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
// Reading the file is common/kv_config.hpp's, and cmake/SiteConfig.cmake and the sweep scripts
// each parse the same subset on their own, so no consumer needs a parser dependency. Those two
// read keys this does not (the RoCE settings), which is why the file holds more than site()
// reports.
//
// Reading a key and trusting it are different things, so nothing here hands back a raw
// value. A declared devdax node that is not a character device, and a declared mount
// directory with nothing mounted on it, both read as absent -- which is what they are, to
// a run that wanted them.

#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "common/kv_config.hpp"

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
        : values_{readOwnStream(path)},
          path_{std::move(path)}
    {
        resolve();
    }

    [[nodiscard]] const Site_t& site() const noexcept
    {
        return site_;
    }

    // Opened here rather than through KeyValueConfig::loadIfPresent, which refuses a file group or
    // other can write. This one is a checkout's own file, and git gives it whatever the developer's
    // umask allows, so that rule would make a 002 umask mean the tests do not run.
    [[nodiscard]] static kvconfig::KeyValueConfig readOwnStream(const std::string& path)
    {
        std::ifstream file{path};
        if (file)
        {
            return kvconfig::KeyValueConfig::parse(file, path);
        }

        // An absent file carries no keys, which is what an empty stream parses to.
        std::istringstream nothing;
        return kvconfig::KeyValueConfig::parse(nothing, path);
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

    void resolve()
    {
        const std::string device = values_.getString("dax_device");
        if (!device.empty() && isCharDevice(device))
        {
            site_.daxDevice = device;
        }

        const std::string dir = values_.getString("file_backend_dir");
        if (!dir.empty() && isMountPoint(dir))
        {
            site_.fileBackendDir = dir;
        }

        site_.daxSlotReserve = values_.getU64("dax_slot_reserve", DefaultSlotReserve);
        site_.daxSlotBase = values_.getU64("dax_slot_base", 0);
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

    kvconfig::KeyValueConfig values_;
    std::string path_;
    Site_t site_{};
};

}  // namespace harness
