// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_scratch.hpp -- one probe run's own directory and shm names, removed when the run ends.
//
// Two probes running at once would otherwise open the same shm name and the same socket, and each
// would see failures the other caused. The pid in every name is what keeps two runs apart.

#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cmed::harness
{

// Where a run puts its files. The environment names it so ctest can point every probe at one place
// outside the source tree; the working directory is what a developer running one probe by hand gets.
[[nodiscard]] inline std::string readScratchRoot()
{
    const char* named = ::getenv("CMED_TEST_SCRATCH");
    return (named != nullptr && named[0] != '\0') ? std::string{named} : std::string{"."};
}

// One run's directory, and the shm names that run created. Both go at the end, so a suite that ran a
// hundred times leaves nothing behind and a run that crashed leaves only its own.
class ProbeScratch
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // @label names the probe. The pid makes the run unique, which is what two probes at once need.
    explicit ProbeScratch(std::string_view label)
        : label_{std::string{label} + "-" + std::to_string(static_cast<std::int64_t>(::getpid()))},
          directory_{std::filesystem::path{readScratchRoot()} / label_}
    {
        std::error_code failed;
        std::filesystem::create_directories(directory_, failed);
    }

    ProbeScratch(const ProbeScratch&) = delete;
    ProbeScratch(ProbeScratch&&) = delete;

    ~ProbeScratch() noexcept
    {
        for (const std::string& name : areaNames_)
        {
            static_cast<void>(::shm_unlink(name.c_str()));
        }

        std::error_code failed;
        static_cast<void>(std::filesystem::remove_all(directory_, failed));
    }

    // ── operator= ──────────────────────────────────────────────────
    ProbeScratch& operator=(const ProbeScratch&) = delete;
    ProbeScratch& operator=(ProbeScratch&&) = delete;

    // ── public methods ─────────────────────────────────────────────
    // An shm name, recorded so the destructor unlinks it. Not a path: shm names live in their own
    // namespace, and a run that names one twice gets the same name back.
    [[nodiscard]] std::string makeAreaName(std::string_view part)
    {
        std::string name = label_;
        if (!part.empty())
        {
            name += "-";
            name += part;
        }

        areaNames_.push_back(name);
        return name;
    }

    // A file inside this run's directory, removed with it. The caller creates it; nothing here does.
    [[nodiscard]] std::string makePath(std::string_view leaf) const
    {
        return (directory_ / leaf).string();
    }

    // ── accessors ──────────────────────────────────────────────────
    // What a config file writes as its socket directory, since the socket belongs to this run too.
    [[nodiscard]] std::string readDirectory() const
    {
        return directory_.string();
    }

    [[nodiscard]] const std::string& readLabel() const noexcept
    {
        return label_;
    }

private:
    std::string label_;
    std::filesystem::path directory_;
    std::vector<std::string> areaNames_;
};

}  // namespace cmed::harness
