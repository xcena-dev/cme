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
#include <exception>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/policy/successor_request_agg_layout.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

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
        sleepMs(100);
        aggregator = readAggregatorPeerId(region, groupId, mode);
    }
    return aggregator;
}

struct PeerSlot_t
{
    std::thread tid;
    std::unique_ptr<cme::Peer> peer;  // WORKER-THREAD-ONLY
    cme::Geometry* region{nullptr};
    cme::PeerId peerId{0};
    cme::CoherencyMode coherency{};
    cme::DomainId numDomains{0};
    std::atomic<bool> stopReq{false};
    std::atomic<bool> freezeReq{false};  // main raises; worker applies to its own Peer
    std::atomic<std::uint64_t> acqCount{0};
    std::atomic<int> rc{0};
};

void worker(PeerSlot_t* slot)
{
    try
    {
        slot->peer = std::make_unique<cme::Peer>(*slot->region, slot->peerId, slot->coherency);
        for (cme::DomainId joinDomainId = 1; joinDomainId <= slot->numDomains; ++joinDomainId)
        {
            slot->peer->joinDomain(joinDomainId);
        }
    }
    catch (const std::exception& error)
    {
        log("peer %u: ctor exception: %s", slot->peerId, error.what());
        slot->rc.store(1);
        return;
    }

    cme::DomainId nextDomainId = 1;
    while (!slot->stopReq.load(std::memory_order_acquire))
    {
        slot->peer->setFreeze(slot->freezeReq.load(std::memory_order_acquire));
        if (slot->freezeReq.load(std::memory_order_acquire))
        {
            sleepMs(50);
            continue;
        }
        try
        {
            auto ownershipGuard = slot->peer->lock(nextDomainId);
            slot->acqCount.fetch_add(1, std::memory_order_relaxed);
            (void)ownershipGuard;
        }
        catch (const cme::LockTimeoutError&)
        {
            // @expected a lock attempt that times out under contention is a normal outcome, not a failure: the loop moves on to the next domain.
        }
        nextDomainId = (nextDomainId % slot->numDomains) + 1;
    }
    slot->peer.reset();  // dtor leaves membership
}

void spawnPeer(PeerSlot_t& slot, cme::PeerId peerId, cme::Geometry* region,
               cme::DomainId numDomains, cme::CoherencyMode coherency)
{
    slot.peerId = peerId;
    slot.region = region;
    slot.coherency = coherency;
    slot.numDomains = numDomains;
    slot.tid = std::thread{worker, &slot};
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    startLogClock();

    // Fixed dims: 2 groups over 4 peers -> group0 = {0,2}, group1 = {1,3}; format
    // seeds each group's aggregator = its first member (group g -> peer g).
    constexpr cme::DomainId NumDomains = 2;
    constexpr cme::PeerId MaxPeers = 4;
    constexpr std::uint32_t Groups = 2;
    constexpr cme::DomainId DomainCeiling = NumDomains + 1;  // + control(0)
    constexpr std::uint32_t RecoveryWaitMs = 6000;

    cme::Geometry::FormatOpts_t fmtOpts{cme::Strategy::RequestAgg};
    fmtOpts.aggregatorGroups = Groups;

    std::optional<cme::Geometry> region;
    region.emplace(ctx.memory().createRegion(DomainCeiling, MaxPeers, fmtOpts));
    seedDataDomains(*region, NumDomains, ctx.coherency());

    // seedDataDomains' transient creator left group0's aggregator at NoPeer, and join does
    // not re-claim the duty. Reset both groups: the re-election needs a live aggregator.
    writeAggregatorPeerId(*region, 0, 0, ctx.coherency());
    writeAggregatorPeerId(*region, 1, 1, ctx.coherency());

    std::vector<PeerSlot_t> peers(MaxPeers);
    log("starting %u peers (request-agg, groups=%u, domains=%u, backend=%s)", MaxPeers, Groups,
        NumDomains, ctx.backendName());
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        spawnPeer(peers[i], i, &*region, NumDomains, ctx.coherency());
    }

    sleepMs(1000);  // steady state

    // Initial: format names group g's aggregator = peer g.
    ctx.check(readAggregatorPeerId(*region, 0, ctx.coherency()) == 0,
              "group0 aggregator starts as peer 0");
    ctx.check(readAggregatorPeerId(*region, 1, ctx.coherency()) == 1,
              "group1 aggregator starts as peer 1");

    // ── crash group0's aggregator (peer 0) ─────────────────────────
    log("freeze peer 0 (group0 aggregator crashes)");
    peers[0].freezeReq.store(true);
    // Poll until recovery re-elects (record no longer names the dead peer 0).
    const cme::PeerId reelected =
        pollAggregatorUntil(*region, 0, ctx.coherency(), RecoveryWaitMs, [](cme::PeerId aggregator)
                            {
                                return aggregator != 0;
                            });
    // group0 = {0,2}; peer 2 is the only other member -> the re-election must pick it.
    ctx.check(reelected == 2, "group0 aggregator re-elected to live member peer 2");
    ctx.check(reelected != 0, "group0 aggregator no longer names the dead peer 0");

    // group1 is untouched -- a crash in group0 must not disturb its record.
    ctx.check(readAggregatorPeerId(*region, 1, ctx.coherency()) == 1,
              "group1 aggregator unchanged");

    // Survivors resume progress once recovery settles. Poll for it -- a peer can stall
    // briefly mid-takeover, so a fixed window right after re-election flakes.
    const std::uint64_t before = peers[2].acqCount.load();
    bool progressed = false;
    for (std::uint32_t waited = 0; waited < RecoveryWaitMs && !progressed; waited += 100)
    {
        sleepMs(100);
        progressed = peers[2].acqCount.load() > before;
    }
    ctx.check(progressed, "group0 survivor peer 2 keeps acquiring");

    // ── crash the re-elected aggregator too: whole group gone -> NoPeer ───
    log("freeze peer 2 (group0's last member crashes)");
    peers[2].freezeReq.store(true);
    const cme::PeerId finalAgg = pollAggregatorUntil(
        *region, 0, ctx.coherency(), RecoveryWaitMs, [](cme::PeerId aggregator)
        {
            return aggregator == cme::NoPeer;
        });
    ctx.check(finalAgg == cme::NoPeer,
              "group0 aggregator = NoPeer once the whole group is gone (holder fallback)");

    // ── shutdown: worker loop re-checks stopReq even while frozen, so all join ──
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        peers[i].stopReq.store(true);
    }
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        if (peers[i].tid.joinable())
        {
            peers[i].tid.join();
        }
    }
    log("shutdown");
}

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, runBody);
}
