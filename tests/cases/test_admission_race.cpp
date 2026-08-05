// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_admission_race.cpp -- concurrent peer-slot claim across processes.
//
// N processes claim against the same region simultaneously. Each must win a
// distinct slot: no collision, no spurious NoFreeSlot when slots >= N. This
// exercises claimPeerSlot's optimistic protocol and, in particular, that a
// losing claimer never resets a slot a winner already staked.
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "admission/claim.hpp"
#include "cme/shared.hpp"
#include "core/layout/geometry.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr std::uint32_t Domains = 1;
constexpr std::uint32_t Claimers = 4;

constexpr std::int32_t Unset = -2;
constexpr std::int32_t Failed = -1;

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const std::string& uri = ctx.uri();

    // Parent formats the region and lets the mapping go; the children each open it for
    // themselves and claim.
    static_cast<void>(ctx.memory().createRegion(
        Domains, Claimers, cme::Geometry::FormatOpts_t{cme::Strategy::Request}));

    const auto results = ctx.scratch<std::int32_t>("results", Claimers);
    if (!results)
    {
        ctx.check(false, "shared result buffer mapped");
        return;
    }
    results.fill(Unset);

    for (std::uint32_t i = 0; i < Claimers; ++i)
    {
        const pid_t pid = ::fork();
        if (pid < 0)
        {
            std::perror("fork");
            ctx.check(false, "fork failed");
            return;
        }
        if (pid == 0)
        {
            std::int32_t claimed = Failed;
            try
            {
                auto region = cme::Geometry::open(uri);
                region.bindBlocking(std::chrono::milliseconds{5000}, ctx.coherency());
                claimed = static_cast<std::int32_t>(cme::admission::claimPeerSlot(region, ctx.coherency()));
            }
            catch (...)
            {
                claimed = Failed;
            }
            results[i] = claimed;
            std::_Exit(0);
        }
    }

    for (std::uint32_t i = 0; i < Claimers; ++i)
    {
        int status = 0;
        ::wait(&status);
    }

    // Every claimer won a distinct, in-range slot.
    bool allInRange = true;
    bool noFailure = true;
    for (std::uint32_t i = 0; i < Claimers; ++i)
    {
        if (results[i] == Failed || results[i] == Unset)
        {
            noFailure = false;
        }
        else if (results[i] < 0 || results[i] >= static_cast<std::int32_t>(Claimers))
        {
            allInRange = false;
        }
    }
    ctx.check(noFailure, "every claimer obtained a slot (no NoFreeSlot / crash)");
    ctx.check(allInRange, "every claimed peerId is in [0, N)");

    bool distinct = true;
    for (std::uint32_t first = 0; first < Claimers; ++first)
    {
        for (std::uint32_t second = first + 1; second < Claimers; ++second)
        {
            if (results[first] == results[second])
            {
                distinct = false;
            }
        }
    }
    ctx.check(distinct, "no two claimers won the same slot");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
