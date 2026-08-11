// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// args.hpp -- `--flag value` lookup over the arguments a program was given.
//
// Here rather than with the test harness because the shipped tools read their own flags the same
// way, and a program a user installs must not link a test harness to do it. The standalone probes
// in bench/ have the mirror requirement: they measure the medium with mmap and intrinsics, so
// pulling libcme in through this would defeat what they exist to measure.
//
// Call takeArgs() first thing in main(). Every later lookup reads what it stored, so no caller has
// to thread argv through.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace cliargs
{

// The arguments takeArgs() stored, minus argv[0].
inline std::vector<std::string>& extraArgs()
{
    static std::vector<std::string> instance;
    return instance;
}

// Store argv for the lookups below. Returns argc unchanged, so a caller that also runs its own
// parse keeps working.
inline int takeArgs(int argc, char** argv)
{
    std::vector<std::string>& args = extraArgs();
    args.clear();
    for (int index = 1; index < argc; ++index)
    {
        args.emplace_back(argv[index]);
    }
    return argc;
}

// Value of `--flag <value>`, or @fallback when the flag is absent or has no value.
inline std::string argStr(const char* flag, const std::string& fallback)
{
    const std::vector<std::string>& args = extraArgs();
    for (std::uint64_t index = 0; index + 1 < args.size(); ++index)
    {
        if (args[index] == flag)
        {
            return args[index + 1];
        }
    }
    return fallback;
}

// Same, parsed as an unsigned decimal. A value that does not parse, or parses to zero, yields
// @fallback: a benchmark given --iters 0 wants the default, not an empty run.
inline std::uint64_t argU64(const char* flag, std::uint64_t fallback)
{
    const std::string raw = argStr(flag, std::string{});
    if (raw.empty())
    {
        return fallback;
    }
    const std::uint64_t parsed = std::strtoull(raw.c_str(), nullptr, 10);
    return (parsed > 0) ? parsed : fallback;
}

// True when @flag is present at all, for options that carry no value.
inline bool argFlag(const char* flag)
{
    const std::vector<std::string>& args = extraArgs();
    for (const std::string& arg : args)
    {
        if (arg == flag)
        {
            return true;
        }
    }
    return false;
}

}  // namespace cliargs
