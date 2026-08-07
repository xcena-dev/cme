// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_agg_recovery.cpp -- RequestAgg aggregator re-election on crash (B1).
//
// A graceful leave hands the aggregator duty off; a crash cannot, so scrubRecoveredPeer
// must re-elect a live group member or the group is stuck in holder-fallback forever. This
// crashes an aggregator and checks the record re-points at a live member, then reaches
// NoPeer once the whole group is gone.
//
// Strategy is fixed to RequestAgg, so --strategy is ignored. The aggregator record is reached
// through RequestAggLayout, the same class the policy uses to write it.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/policy/successor_request_agg_layout.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

// Where the record sits is successor_request_agg_layout.hpp's answer, so this test cannot
// drift from the policy that writes it. Read through coherency::get rather than a bare load:
// on uc and dax a raw read is missing the fence the policy paired its write with.
[[nodiscard]] cme::PeerId readAggregatorPeerId(cme::Geometry& region, std::uint32_t groupId,
                                               cme::CoherencyMode mode)
{
    const cme::RequestAggLayout layout{region};
    return static_cast<cme::PeerId>(
        cme::coherency::get(layout.getRecord(groupId), mode).aggregatorPeerId);
}

void writeAggregatorPeerId(cme::Geometry& region, std::uint32_t groupId, cme::PeerId peerId,
                           cme::CoherencyMode mode)
{
    const cme::RequestAggLayout layout{region};
    cme::RequestAggLayout::AggregatorRecord_t record{};
    record.magic = cme::RequestAggLayout::AggregatorRecord_t::Magic;
    record.aggregatorPeerId = peerId;
    cme::coherency::set(layout.getRecord(groupId), record, mode);
}

// Poll group @groupId's aggregator until @done(value) or @maxMs elapses; returns the
// last read. Beats a fixed sleep -- recovery latency varies with dax load, so a single
// post-sleep read flakes when the re-election lands a hair late.
template <typename T>
cme::PeerId pollAggregatorUntil(cme::Geometry& region, std::uint32_t groupId,
                                cme::CoherencyMode mode, std::uint32_t maxMs, T done)
{
    cme::PeerId aggregator = readAggregatorPeerId(region, groupId, mode);
    for (std::uint32_t waited = 0; waited < maxMs && !done(aggregator); waited += 100)
    {
        harness::sleepMs(100);
        aggregator = readAggregatorPeerId(region, groupId, mode);
    }
    return aggregator;
}

// The other member of @peerId's group, with two groups over four peers: group g = {g, g + 2}.
[[nodiscard]] cme::PeerId groupPartner(cme::PeerId peerId) noexcept
{
    return static_cast<cme::PeerId>(peerId < 2 ? peerId + 2 : peerId - 2);
}

// A group's aggregator is re-elected only while its members keep asking, so the frozen worker here
// idles longer than the harness default: this case watches the region rather than a thaw.
constexpr std::uint32_t FrozenSleepMs = 50;

}  // namespace

void runBody(harness::TestContext& ctx)
{
    harness::startLogClock();

    // Fixed dims: 2 groups over 4 peers -> group0 = {0,2}, group1 = {1,3}.
    constexpr cme::DomainId NumDomains = 2;
    constexpr cme::PeerId MaxPeers = 4;
    constexpr std::uint32_t Groups = 2;
    constexpr cme::DomainId DomainCeiling = NumDomains + 1;  // + control(0)
    constexpr std::uint32_t RecoveryWaitMs = 6000;

    cme::Geometry::FormatOpts_t fmtOpts{cme::Strategy::RequestAgg};
    fmtOpts.aggregatorGroups = Groups;

    auto region = ctx.memory().createRegion(DomainCeiling, MaxPeers, fmtOpts);
    harness::seedDataDomains(region, NumDomains);

    // seedDataDomains' transient creator leaves group0's aggregator at NoPeer, and the crash below
    // needs a named holder. Which member ends up holding it is settled by the peers, not here.
    writeAggregatorPeerId(region, 0, 0, ctx.coherency());
    writeAggregatorPeerId(region, 1, 1, ctx.coherency());

    std::vector<harness::PeerSlot_t> peers(MaxPeers);
    harness::log("starting %u peers (request-agg, groups=%u, domains=%u, backend=%s)", MaxPeers, Groups,
                 NumDomains, ctx.backendName());
    for (harness::PeerSlot_t& slot : peers)
    {
        slot.idleMs = FrozenSleepMs;
    }
    harness::spawnPeerWorkers(peers, MaxPeers, region, NumDomains);

    harness::sleepMs(1000);  // steady state
    ctx.check(harness::allPeersJoined(peers, MaxPeers), "every worker joined its domains");

    // The duty goes to whichever member first finds it named on a peer that is not Active, and
    // these records are seeded before any peer exists, so both members of a group race for it.
    // Read the winner rather than name one; everything below works from it.
    const cme::PeerId group0Start = readAggregatorPeerId(region, 0, ctx.coherency());
    const cme::PeerId group1Start = readAggregatorPeerId(region, 1, ctx.coherency());
    ctx.checkf(group0Start == 0 || group0Start == 2,
               "group0 aggregator starts on one of its members (peer %u)", group0Start);
    ctx.checkf(group1Start == 1 || group1Start == 3,
               "group1 aggregator starts on one of its members (peer %u)", group1Start);

    // ── crash group0's aggregator, whichever member that is ────────
    const cme::PeerId group0Survivor = groupPartner(group0Start);
    harness::log("freeze peer %u (group0 aggregator crashes; peer %u survives)", group0Start,
                 group0Survivor);
    peers[group0Start].frozen.store(true);
    // Poll until recovery re-elects: the record no longer names the frozen peer.
    const cme::PeerId reelected = pollAggregatorUntil(
        region, 0, ctx.coherency(), RecoveryWaitMs, [group0Start](cme::PeerId aggregator)
        {
            return aggregator != group0Start;
        });
    // A group has two members, so the survivor is the only candidate the re-election can pick.
    ctx.checkf(reelected == group0Survivor, "group0 aggregator re-elected to live member peer %u",
               group0Survivor);

    // A crash in group0 must not disturb group1's record. Against the observed value, not a
    // literal, since the winner above is not fixed.
    const cme::PeerId group1After = readAggregatorPeerId(region, 1, ctx.coherency());
    ctx.checkf(group1After == group1Start, "group1 aggregator unchanged (peer %u)", group1Start);

    // Survivors resume progress once recovery settles. Poll for it -- a peer can stall
    // briefly mid-takeover, so a fixed window right after re-election flakes.
    const std::uint64_t before = peers[group0Survivor].acquires.load();
    const bool progressed = harness::waitUntil(
        [&peers, group0Survivor, before]
        {
            return peers[group0Survivor].acquires.load() > before;
        },
        RecoveryWaitMs, 100);
    ctx.checkf(progressed, "group0 survivor peer %u keeps acquiring", group0Survivor);

    // ── crash the re-elected aggregator too: whole group gone -> NoPeer ───
    harness::log("freeze peer %u (group0's last member crashes)", group0Survivor);
    peers[group0Survivor].frozen.store(true);
    const cme::PeerId finalAgg = pollAggregatorUntil(
        region, 0, ctx.coherency(), RecoveryWaitMs, [](cme::PeerId aggregator)
        {
            return aggregator == cme::NoPeer;
        });
    ctx.check(finalAgg == cme::NoPeer,
              "group0 aggregator = NoPeer once the whole group is gone (holder fallback)");

    // ── shutdown: the worker loop re-checks stop even while frozen, so all join ──
    harness::joinPeerWorkers(peers, MaxPeers);
    harness::log("shutdown");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
