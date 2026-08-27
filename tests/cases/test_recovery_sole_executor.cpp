// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_recovery_sole_executor.cpp -- one dead peer is recovered by exactly one authority (R2).
//
// A second executor would seize a domain the first already re-seized, which is split ownership.
// The count is RecoveryCompleted, emitted once per executor; rounds, because a fork is a race.
// Only CME_STATS bumps it, so a build without it leaves by SKIP rather than asserting nothing.

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

// Six peers so three can die and three still watch; a recovery needs a live RA.
constexpr cme::PeerId MaxPeers = 6;
constexpr cme::DomainId NumDomains = 2;
constexpr cme::DomainId Ceiling = NumDomains + 1;  // control + data
constexpr cme::PeerId Rounds = 3;                  // peers 1..3 die, 0 and 4..5 survive throughout

// Recovery is wall-clock-timed, so poll the post-condition rather than sleep once and read.
constexpr std::uint32_t RecoveryDeadlineMs = 20000;

// A second executor emits its RecoveryCompleted after the slot already reads None, so sampling at
// None alone would read one and call it unique.
constexpr std::uint32_t ForkWindowMs = 1500;

[[nodiscard]] std::uint64_t
totalCompleted(const std::array<harness::PeerSlot_t, MaxPeers>& peers)
{
    std::uint64_t total = 0;
    for (cme::PeerId index = 0; index < MaxPeers; ++index)
    {
        total += harness::readTelemetry(peers[index]).recovery.completed;
    }
    return total;
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    auto region = harness::createRegion(Ceiling, MaxPeers);
    harness::seedDataDomains(region, NumDomains);

    std::printf("sole executor: %u peers, %u rounds (%s, backend=%s)\n",
                MaxPeers, Rounds, ctx.strategySuffix(), ctx.backendName());

    std::array<harness::PeerSlot_t, MaxPeers> peers{};
    harness::spawnPeerWorkers(peers, MaxPeers, region, NumDomains);
    harness::sleepMs(1000);  // memberships go Active; ownership spreads
    if (!ctx.check(harness::allPeersJoined(peers, MaxPeers), "every worker joined its domains"))
    {
        harness::joinPeerWorkers(peers, MaxPeers);
        return;
    }

    // Asked past allPeersJoined, since a worker whose Peer never came up reads as not counting.
    const bool counting = harness::readTelemetry(peers[0]).countersLive;

    bool freedEvery = true;
    bool soleEvery = true;
    for (cme::PeerId round = 0; round < Rounds; ++round)
    {
        const cme::PeerId dead = round + 1;  // peer 0 stays, so the ring always has a watcher below
        const std::uint64_t before = totalCompleted(peers);

        peers[dead].frozen.store(true);
        peers[dead].abandon.store(true);  // no destructor over a slot recovery has freed

        const bool freed = harness::waitUntil(
            [&region, dead]
            {
                return harness::hasMemberStatus(region, dead, Status::None);
            },
            RecoveryDeadlineMs);
        freedEvery = freedEvery && freed;
        if (!freed)
        {
            break;  // nothing recovered, so a count of zero says nothing about uniqueness
        }

        harness::sleepMs(ForkWindowMs);  // let a second executor show up if there is one
        const std::uint64_t executed = totalCompleted(peers) - before;
        soleEvery = soleEvery && executed == 1;
        std::printf("round %u: peer %u recovered by %" PRIu64 " authority\n", round, dead, executed);
    }

    ctx.check(freedEvery, "every round's dead peer had its slot freed");
    harness::joinPeerWorkers(peers, MaxPeers);

    // R2 is the subject and the counter is the only place it shows, so a build that keeps no
    // counters leaves by SKIP rather than reporting the green of a run that asserted it. A failure
    // already on the tally outranks that: SKIP would take it off.
    if (!counting)
    {
        if (ctx.failures() == 0)
        {
            harness::TestContext::skip("built without CME_STATS, so no RecoveryCompleted is counted");
        }
        return;
    }
    ctx.check(soleEvery, "every round was carried through by exactly one authority");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
