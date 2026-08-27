// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_admission_stress.cpp -- peer-slot claim under sustained contention.
//
// claimPeerSlot is the only genuinely multi-writer race in cme -- every other mutation is
// serialised by a control- or domain-ME lock. This drives it over many rounds, with a spin
// barrier so the claims of one round land in the narrowest window.
//   Part 1 (exact fit): N claimers vs N slots. All N win distinct in-range slots.
//   Part 2 (over-subscription): M > N claimers. Exactly N win, the surplus get
//     NoFreeSlotError, and no two winners share a slot.
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "admission/claim.hpp"
#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/layout/geometry.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr std::uint32_t Slots = 6;     // maxPeers per round
constexpr std::uint32_t Oversub = 10;  // claimers in the over-subscription part
constexpr std::uint32_t Rounds = 40;   // rounds per part
constexpr std::uint32_t Domains = 1;

constexpr std::int32_t Unset = -2;
constexpr std::int32_t Failed = -1;

// Fork @numClaimers children that open, wait on the shared barrier, then claim once each;
// results[] carries the won peerId or Failed. The parent releases the barrier only after
// every child is forked, so the claims contend in the tightest window we can arrange.
void runRound(const std::string& uri, std::uint32_t numClaimers, std::int32_t* results, std::int32_t* barrier,
              cme::CoherencyMode coherency)
{
    for (std::uint32_t i = 0; i < numClaimers; ++i)
    {
        results[i] = Unset;
    }
    __atomic_store_n(barrier, 0, __ATOMIC_RELEASE);

    const std::uint32_t spawned = harness::spawnChildren(
        numClaimers,
        [&uri, results, barrier, coherency](std::uint32_t index)
        {
            std::int32_t claimed = Failed;
            try
            {
                auto region = cme::Geometry::open(uri);
                region.bindBlocking(timing::Secs{5}, coherency);
                while (__atomic_load_n(barrier, __ATOMIC_ACQUIRE) == 0)
                {
                    // spin to the barrier
                }
                claimed = static_cast<std::int32_t>(cme::admission::claimPeerSlot(region, coherency));
            }
            catch (...)
            {
                claimed = Failed;  // NoFreeSlotError or any bind/open failure
            }
            results[index] = claimed;
        });

    __atomic_store_n(barrier, 1, __ATOMIC_RELEASE);  // release all claimers together

    harness::reapChildren(spawned);
    if (spawned != numClaimers)
    {
        std::exit(2);  // a round short of its claimers proves nothing about the race
    }
}

// Among results[0..numClaimers), count winners + verify distinct & in-range.
// Returns the winner count; sets *valid false on any out-of-range or duplicate.
std::uint32_t auditWinners(const std::int32_t* results, std::uint32_t numClaimers, bool* valid)
{
    std::uint32_t winners = 0;
    for (std::uint32_t i = 0; i < numClaimers; ++i)
    {
        if (results[i] == Unset)
        {
            *valid = false;  // child never recorded -> crash
            continue;
        }
        if (results[i] == Failed)
        {
            continue;
        }
        ++winners;
        if (results[i] < 0 || results[i] >= static_cast<std::int32_t>(Slots))
        {
            *valid = false;
        }
        for (std::uint32_t j = i + 1; j < numClaimers; ++j)
        {
            if (results[j] == results[i])
            {
                *valid = false;  // two winners share a slot
            }
        }
    }
    return winners;
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const std::string& uri = ctx.uri();

    const auto results = ctx.scratch<std::int32_t>("results", Oversub);
    const auto barrier = ctx.scratch<std::int32_t>("barrier", 1);
    if (!results || !barrier)
    {
        ctx.check(false, "shared result buffer and barrier mapped");
        return;
    }

    // ── Part 1: exact fit (N claimers, N slots), repeated ──────────────
    bool p1ok = true;
    bool p1full = true;
    for (std::uint32_t round = 0; round < Rounds; ++round)
    {
        ctx.memory().remove();
        // Formats and lets the mapping go: the parent only lays the region down, and the
        // children each open it for themselves.
        static_cast<void>(harness::createRegion(Domains, Slots));
        runRound(uri, Slots, results.data(), barrier.data(), ctx.coherency());

        bool valid = true;
        const std::uint32_t winners = auditWinners(results.data(), Slots, &valid);
        if (!valid)
        {
            p1ok = false;
        }
        if (winners != Slots)
        {
            p1full = false;  // exact fit -> everyone must win
        }
    }
    ctx.check(p1ok, "exact-fit: every round's winners distinct + in-range (no collision)");
    ctx.check(p1full, "exact-fit: all N claimers won each round (no spurious NoFreeSlot)");

    // ── Part 2: over-subscription (M > N claimers, N slots), repeated ──
    bool p2ok = true;
    bool p2exact = true;
    for (std::uint32_t round = 0; round < Rounds; ++round)
    {
        ctx.memory().remove();
        // Formats and lets the mapping go: the parent only lays the region down, and the
        // children each open it for themselves.
        static_cast<void>(harness::createRegion(Domains, Slots));
        runRound(uri, Oversub, results.data(), barrier.data(), ctx.coherency());

        bool valid = true;
        const std::uint32_t winners = auditWinners(results.data(), Oversub, &valid);
        if (!valid)
        {
            p2ok = false;
        }
        if (winners != Slots)
        {
            p2exact = false;  // exactly N win, surplus get NoFreeSlot
        }
    }
    ctx.check(p2ok, "over-subscription: winners distinct + in-range, no two share a slot");
    ctx.check(p2exact, "over-subscription: exactly N winners each round (surplus -> NoFreeSlot)");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
