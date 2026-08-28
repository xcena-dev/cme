// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_medium.hpp -- which medium a probe that starts a real daemon runs on.
//
// The cme harness turns a medium name and a window index into a URI by reading the site config at
// run time, so no device path reaches the build files. Separate from helper.hpp because it drags in
// libcme, the same reason helper_cme_region.hpp is separate.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "cme/shared.hpp"
#include "config_reader.hpp"
#include "test_memory.hpp"

namespace cmed::harness
{

// The cme harness, qualified from the root: an unqualified harness:: inside this namespace finds
// this one instead, because cmed::harness is what enclosing-scope lookup reaches first.
namespace cmeh = ::harness;

// What a daemon config's region.coherency line calls each mode. The daemon maps the name back with
// a table of its own, so a name this does not produce is refused there rather than here.
[[nodiscard]] inline const char* coherencyName(cme::CoherencyMode mode) noexcept
{
    switch (mode)
    {
        case cme::CoherencyMode::Uncached:
            return "uncached";
        case cme::CoherencyMode::Flush:
            return "flush";
        default:
            return "cache_coherent";
    }
}

// The same map read backwards, for a probe that opens the region itself. A bare open defaults to
// CacheCoherent, which is the wrong barrier discipline on devdax and on an uncacheable mount.
[[nodiscard]] inline cme::CoherencyMode coherencyFromName(std::string_view name) noexcept
{
    if (name == "uncached")
    {
        return cme::CoherencyMode::Uncached;
    }
    if (name == "flush")
    {
        return cme::CoherencyMode::Flush;
    }
    return cme::CoherencyMode::CacheCoherent;
}

// The flags every probe that opens a region through a daemon takes.
struct MediumOptions_t
{
    std::string backendName{"shm"};
    std::uint64_t slot{0};
    std::string caseName{"cmed_probe"};
    std::string configPath{cmeh::findSiteConfig()};
    std::string uri;
    std::string coherency{"cache_coherent"};

    // Flag and value in pairs, which is what ctest passes and what each probe's own loop already
    // reads. A flag this does not know is left for that loop.
    void parse(int argc, char** argv)
    {
        for (int index = 1; index + 1 < argc; index += 2)
        {
            const std::string_view flag{argv[index]};
            const char* value = argv[index + 1];
            if (flag == "--backend")
            {
                backendName = value;
            }
            else if (flag == "--slot")
            {
                slot = std::strtoull(value, nullptr, 0);
            }
            else if (flag == "--case")
            {
                caseName = value;
            }
            else if (flag == "--config")
            {
                configPath = value;
            }
            else if (flag == "--uri")
            {
                uri = value;
            }
            else if (flag == "--coherency")
            {
                coherency = value;
            }
        }
    }

    // One area on the medium --backend names, cleared of whatever a previous run left, or a thrown
    // MediumUnavailable. @label separates two areas of one run; @window offsets the device slot.
    [[nodiscard]] std::unique_ptr<cmeh::TestMemory> openArea(const std::string& label,
                                                             std::uint64_t window) const
    {
        const cmeh::ConfigReader config{configPath};
        const std::string runName = label.empty() ? caseName : caseName + "_" + label;
        return cmeh::TestMemory::open(config, cmeh::backendFromName(backendName), runName,
                                      slot + window);
    }

    // Settles uri and coherency from that medium and hands back the area holding them. Hands back
    // nothing when --uri named a medium whole: the caller owns that one, down to its mode.
    [[nodiscard]] std::unique_ptr<cmeh::TestMemory> resolve()
    {
        if (!uri.empty())
        {
            return nullptr;
        }

        std::unique_ptr<cmeh::TestMemory> area = openArea({}, 0);
        uri = area->uri();
        coherency = coherencyName(area->coherency());
        return area;
    }

    [[nodiscard]] cme::CoherencyMode coherencyMode() const noexcept
    {
        return coherencyFromName(coherency);
    }
};

}  // namespace cmed::harness
