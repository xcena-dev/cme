// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_peer_bound.cpp -- each join sets the peer bound one past the highest slot in use, through churn.
//
// Admission gives each joiner the lowest free slot and sets the bound past the highest slot in use; a
// leave alone moves nothing. The case drives joins and leaves in a fixed pseudo-random order, predicts
// each joiner's slot from its own record of the slots it holds, and checks the bound after every step.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iterator>
#include <optional>
#include <vector>

#include "cme/shared.hpp"
#include "common/ordered.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

// The smallest region format accepts: the bound is the subject, and no data domain bears on it.
constexpr std::uint32_t Domains = 2;
constexpr std::uint32_t Peers = 8;

constexpr std::uint32_t ChurnSteps = 64;
// The churn holds at most half the slots, so the bound it leaves is below the one the parallel joins reach.
constexpr std::uint32_t ChurnSlots = Peers / 2;
constexpr std::uint64_t ChurnSeed = 0x9E3779B97F4A7C15ULL;
// How long a parallel joiner waits for the others before it leaves, so all hold a slot at once.
constexpr std::uint32_t GatherWaitMs = 5000;

[[nodiscard]] std::uint32_t readPeerBound(const cme::Geometry& region)
{
    return cme::coherency::get(region.getPeerBound(), harness::currentRun().coherency()).peerScanBound;
}

// The slot admission hands the next joiner: the lowest one this case does not hold.
[[nodiscard]] cme::PeerId findLowestFree(const std::vector<std::optional<cme::Session>>& held)
{
    const auto free = std::find_if(held.begin(), held.end(), [](const auto& session)
                                   {
                                       return !session.has_value();
                                   });
    return static_cast<cme::PeerId>(std::distance(held.begin(), free));
}

// The highest slot this case holds, or NoPeer when it holds none.
[[nodiscard]] cme::PeerId findHighestHeld(const std::vector<std::optional<cme::Session>>& held)
{
    const auto last = std::find_if(held.rbegin(), held.rend(), [](const auto& session)
                                   {
                                       return session.has_value();
                                   });
    return last == held.rend() ? cme::NoPeer : static_cast<cme::PeerId>(std::distance(last, held.rend()) - 1);
}

// The @pick-th held slot, counting only the held ones.
[[nodiscard]] cme::PeerId findHeld(const std::vector<std::optional<cme::Session>>& held, std::uint64_t pick)
{
    for (cme::PeerId slot = 0; slot < held.size(); ++slot)
    {
        if (held[slot].has_value() && pick-- == 0)
        {
            return slot;
        }
    }
    return cme::NoPeer;
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    harness::formatSession(Domains, Peers);
    const cme::Geometry region = harness::openBoundRegion();
    ctx.check(readPeerBound(region) == 0, "a fresh region bounds no slot");

    std::vector<std::optional<cme::Session>> held(Peers);
    std::uint32_t expectedBound = 0;
    std::uint64_t rng = ChurnSeed;
    std::uint32_t joins = 0;
    std::uint32_t leaves = 0;
    bool everySlotPredicted = true;
    bool boundAlwaysExact = true;
    for (std::uint32_t step = 0; step < ChurnSteps; ++step)
    {
        const auto heldCount = static_cast<std::uint64_t>(
            std::count_if(held.begin(), held.end(), [](const auto& session)
                          {
                              return session.has_value();
                          }));
        const bool isJoin = heldCount == 0 || (heldCount < ChurnSlots && harness::nextRandom(rng) % 2 == 0);
        if (isJoin)
        {
            const cme::PeerId slot = findLowestFree(held);
            held[slot].emplace(harness::openSession());
            everySlotPredicted = harness::hasMemberStatus(region, slot, Status::Active) && everySlotPredicted;
            expectedBound = findHighestHeld(held) + 1;
            ++joins;
        }
        else
        {
            held[findHeld(held, harness::nextRandom(rng) % heldCount)].reset();
            ++leaves;
        }
        boundAlwaysExact = readPeerBound(region) == expectedBound && boundAlwaysExact;
    }
    std::printf("churn: %u joins, %u leaves, bound %u\n", joins, leaves, expectedBound);
    ctx.check(everySlotPredicted, "every joiner took the lowest free slot");
    ctx.check(boundAlwaysExact, "after every join the bound is one past the highest slot in use, and a leave moves nothing");

    held.clear();
    ctx.check(readPeerBound(region) == expectedBound, "every member leaving moves nothing either");

    ordered::Published<std::uint32_t> gathered{0};
    ordered::Relaxed<std::uint32_t> failed{0};
    const auto isGathered = [&]
    {
        return gathered.load() == Peers;
    };
    harness::runThreads(Peers,
                        [&](std::uint32_t /*index*/)
                        {
                            try
                            {
                                auto session = harness::openSession();
                                gathered.add(1);
                                static_cast<void>(harness::waitUntil(isGathered, GatherWaitMs));
                            }
                            catch (const std::exception& failure)
                            {
                                failed.add(1);
                                harness::log("parallel join failed: %s", failure.what());
                            }
                        });
    ctx.check(failed.load() == 0 && isGathered(),
              "joiners admitted together each got a slot");
    ctx.check(readPeerBound(region) == Peers, "joiners admitted together grow the bound over every slot");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
