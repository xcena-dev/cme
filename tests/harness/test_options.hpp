// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_options.hpp -- the flags one run was given.
//
// Per-run facts arrive as command-line arguments; per-machine facts come from config.yaml
// and live in config_reader.hpp. Nothing is read from the environment.
//
// Set by ctest, one registration at a time:
//   --backend shm|dax|uc   which medium this registration exercises
//   --slot N               dax window index, so dax runs never share a byte
//   --case NAME            unique id for this registration; names the region
//   --strategy NAME        successor policy: order|request|request-agg|peterson
//   --config PATH          override where config.yaml is read from
//   --cleanup              take this registration's uc file away and stop

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "cme/shared.hpp"
#include "common/args.hpp"
#include "config_reader.hpp"
#include "test_memory.hpp"

namespace harness
{

// ctest reads this as "skipped" via SKIP_RETURN_CODE. A machine with no devdax device, or
// no uncacheable mount, skips those registrations instead of failing them: absent hardware
// is not a defect in the code under test.
inline constexpr int SkipExitCode = 77;

// --cleanup asks the binary to take this registration's uc file away and stop. ctest
// registers that as its own entry, so it is a mode of the program rather than a run that
// failed. Deliberately not a std::exception: nothing may mistake it for one.
struct CleanupRequested_t
{
};

// The build cannot answer this case's question: no CME_STATS to count with, no CME_FAILPOINT to
// crash at. MediumUnavailable is the same verdict about hardware. Not a std::exception, as above.
struct SkipRequested_t
{
    const char* reason;
};

// The successor policy and the suffix that tells one registration's region from another's.
struct StrategyChoice_t
{
    cme::Strategy strategy;
    const char* suffix;
};

struct Options_t
{
    Backend backend = Backend::Shm;
    std::uint64_t slot = 0;
    std::string caseName = "cme_test";
    std::string strategy = "request";
    std::string configPath = findSiteConfig();
    bool cleanup = false;

    // Takes these flags out of argv and returns the count that is left, so a run's own
    // getopt_long never meets a flag it was never told about: test_fairness and friends
    // print usage and exit on a long option they were not given.
    //
    // Reads nothing but argv. Where the run then goes is config_reader's business.
    [[nodiscard]] int parse(int argc, char** argv)
    {
        std::vector<char*> kept;
        kept.push_back(argv[0]);

        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            const bool hasValue = (i + 1 < argc);
            if (arg == "--backend" && hasValue)
            {
                backend = backendFromName(argv[++i]);
            }
            else if (arg == "--slot" && hasValue)
            {
                slot = std::strtoull(argv[++i], nullptr, 0);
            }
            else if (arg == "--case" && hasValue)
            {
                caseName = argv[++i];
            }
            else if (arg == "--strategy" && hasValue)
            {
                strategy = argv[++i];
            }
            else if (arg == "--config" && hasValue)
            {
                configPath = argv[++i];
            }
            else if (arg == "--cleanup")
            {
                cleanup = true;
            }
            else
            {
                kept.push_back(argv[i]);
            }
        }

        for (std::size_t i = 0; i < kept.size(); ++i)
        {
            argv[i] = kept[i];
        }
        argv[kept.size()] = nullptr;

        cliargs::extraArgs().assign(kept.begin() + 1, kept.end());
        return static_cast<int>(kept.size());
    }

    // Maps --strategy to a Strategy and a name suffix. Defaults to request, so the recovery
    // and orphan cases run under every successor policy from one binary.
    [[nodiscard]] StrategyChoice_t strategyChoice() const
    {
        if (strategy == "order")
        {
            return {cme::Strategy::Order, "order"};
        }
        if (strategy == "request-agg")
        {
            return {cme::Strategy::RequestAgg, "request_agg"};
        }
        if (strategy == "peterson")
        {
            return {cme::Strategy::Peterson, "peterson"};
        }
        return {cme::Strategy::Request, "request"};
    }
};

// The policy's name for a reader. Distinct from strategyChoice's suffix on purpose: that one
// names a region and goes in a path, so RequestAgg is request_agg there and request-agg here.
[[nodiscard]] inline const char* strategyName(cme::Strategy strategy) noexcept
{
    switch (strategy)
    {
        case cme::Strategy::Order:
            return "order";
        case cme::Strategy::RequestAgg:
            return "request-agg";
        case cme::Strategy::Peterson:
            return "peterson";
        default:
            return "request";
    }
}

}  // namespace harness
