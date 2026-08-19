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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "admission/claim.hpp"
#include "common/timing.hpp"
#include "helper.hpp"
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
    // Parent formats the region and lets the mapping go; the children each open it for
    // themselves and claim.
    static_cast<void>(harness::createRegion(Domains, Claimers));

    const auto results = ctx.scratch<std::int32_t>("results", Claimers);
    if (!results)
    {
        ctx.check(false, "shared result buffer mapped");
        return;
    }
    results.fill(Unset);

    const std::uint32_t spawned = harness::spawnChildren(
        Claimers,
        [&ctx, &results](std::uint32_t index)
        {
            std::int32_t claimed = Failed;
            try
            {
                auto region = harness::openBoundRegion(timing::Secs{5});
                claimed = static_cast<std::int32_t>(cme::admission::claimPeerSlot(region, ctx.coherency()));
            }
            catch (...)
            {
                claimed = Failed;
            }
            results[index] = claimed;
        });

    harness::reapChildren(spawned);
    if (!ctx.check(spawned == Claimers, "every claimer forked"))
    {
        return;
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
