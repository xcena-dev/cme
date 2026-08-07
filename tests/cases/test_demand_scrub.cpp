// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_demand_scrub.cpp -- recovery clears a dead requester's demand line.
//
// The R3 row "stale policy-private state blocks progress after recovery" is closed by a per-policy
// recovery hook. agg_recovery covers RequestAgg, where the private state is the aggregator record.
// Request's private state is the demand line, and no case touched it: a grep for demand across
// tests/cases returned nothing before this one.
//
// What a stale bit costs: the holder's grant scan reads every peer's demand line, so a bit left by
// a peer that died mid-request keeps naming a corpse as a candidate. The domain is then handed to a
// slot that will never take it.
//
// The residue is written directly rather than raced into place. setFreeze is applied by the worker
// between iterations, so a frozen peer is never stopped inside lock() with its bit already up, and
// waiting for that window would make the case a coin flip. recovery_stake_gate builds its claim-word
// residue the same way and for the same reason.
//
// request only. Order and Peterson have no demand region at all, so getSuccessorAreaBase is null
// there and both the read and the write below would be no-ops asserting nothing.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "core/algo/peer.hpp"
#include "core/domain_bitmap.hpp"
#include "core/layout/geometry.hpp"
#include "core/policy/request_demand_region.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

constexpr cme::PeerId MaxPeers = 4;
constexpr cme::DomainId NumDomains = 2;
constexpr cme::DomainId Ceiling = NumDomains + 1;  // control + data
constexpr cme::PeerId Dead = 1;                    // the requester that dies mid-request
constexpr cme::DomainId Wanted = 1;                // the domain its demand bit names

// Recovery is wall-clock-timed (liveness grace + takeover + the policy hook); poll the
// post-condition rather than sleep once and read.
constexpr std::uint32_t RecoveryDeadlineMs = 20000;

// How long the survivors get to show progress once recovery has finished. Generous, because it
// bounds a wait rather than defining a window: the check ends the moment every one of them moves.
constexpr std::uint32_t ProgressDeadlineMs = 5000;

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();

    auto region = harness::createRegion(Ceiling, MaxPeers);
    harness::seedDataDomains(region, NumDomains);

    std::printf("demand scrub: %u peers (%s, backend=%s)\n",
                MaxPeers, ctx.strategySuffix(), ctx.backendName());

    std::array<harness::PeerSlot_t, MaxPeers> peers{};
    harness::spawnPeerWorkers(peers, MaxPeers, region, NumDomains);
    harness::sleepMs(1000);  // memberships go Active; ownership spreads
    ctx.check(harness::allPeersJoined(peers, MaxPeers), "every worker joined its domains");

    const auto demandOf = [&region, coherency](cme::PeerId peerId)
    {
        return cme::request_demand::loadPending(region.getSuccessorAreaBase(), peerId, coherency);
    };
    if (!ctx.check(region.getSuccessorAreaBase() != nullptr,
                   "the strategy has a demand region to scrub"))
    {
        harness::joinPeerWorkers(peers, MaxPeers);
        return;
    }

    // Crash the requester, then leave its request behind. Inside the liveness grace window, so no
    // survivor has started recovery yet and the bit is what a mid-request death leaves.
    peers[Dead].frozen.store(true);
    harness::sleepMs(150);  // the worker applies setFreeze and stops stamping
    cme::DomainBitmap wanted;
    wanted.set(Wanted);
    cme::request_demand::storePending(region.getSuccessorAreaBase(), Dead, wanted, coherency);

    ctx.check(demandOf(Dead).has(Wanted), "the dead peer's demand line carries its request");
    ctx.check(harness::hasMemberStatus(region, Dead, Status::Active),
              "its slot is still Active, so recovery has not run yet");

    const bool scrubbed = harness::waitUntil([&demandOf]
                                             {
                                                 return !demandOf(Dead).has(Wanted);
                                             },
                                             RecoveryDeadlineMs);
    ctx.check(scrubbed, "recovery cleared the dead peer's demand line");
    ctx.check(harness::hasMemberStatus(region, Dead, Status::None),
              "and finished the slot it belonged to");

    // The survivors kept acquiring across the recovery, so the scrub did not come at the cost of
    // the domain the dead peer was asking for.
    //
    // Waited for rather than sampled after a fixed sleep. An acquire on a flushed medium costs
    // enough that a window wide enough to be safe there would also be wide enough to hide a stall,
    // and the first draft of this check failed once on devdax at 500 ms while passing alone.
    std::array<std::uint64_t, MaxPeers> before{};
    for (cme::PeerId index = 0; index < MaxPeers; ++index)
    {
        before[index] = peers[index].acquires.load();
    }
    const bool advanced = harness::waitUntil(
        [&peers, &before]
        {
            for (cme::PeerId index = 0; index < MaxPeers; ++index)
            {
                if (index != Dead && peers[index].acquires.load() <= before[index])
                {
                    return false;
                }
            }
            return true;
        },
        ProgressDeadlineMs);
    ctx.check(advanced, "every survivor advanced after recovery");

    // The crashed peer never rejoins, so its worker leaks the Peer rather than running a
    // destructor over the slot recovery has already finished.
    peers[Dead].abandon.store(true);
    harness::joinPeerWorkers(peers, MaxPeers);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
