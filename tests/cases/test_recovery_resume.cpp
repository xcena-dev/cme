// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_recovery_resume.cpp -- a peer stranded in Recovering by an RA that died
// mid-recovery is resumed to None by a surviving RA (dead-RA resume gap).
//
// The gap: the target is marked Recovering, hence dropped from isAlive, BEFORE the commit
// to None. A ring walk that selects only ALIVE peers can never re-select it, so an RA that
// crashes in that window strands the target forever.
//
// The real window is tens of us, so the stranded state is built directly: freeze the
// target, then flip its slot to Recovering with its claim slot left at NoPeer -- exactly
// what a dead RA leaves behind. A survivor must detect the strand and drive it to None;
// without the fix the slot stays Recovering and this test times out.
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.
// --strategy selects the strategy.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
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

// Recovery is wall-clock-timed (liveness grace + takeover); poll the post-condition.
constexpr std::uint32_t RecoveryDeadlineMs = 15000;

// Minimal lock-loop worker (mirrors recovery_test): joins the data domains and keeps
// acquiring so ownership spreads and the frozen peer likely holds a domain at crash.
struct PeerSlot_t
{
    std::thread tid;
    std::unique_ptr<cme::Peer> peer;  // worker-thread-owned; main drives freeze via freezeReq
    cme::Geometry* region{nullptr};
    cme::PeerId peerId{0};
    cme::CoherencyMode coherency{};
    cme::DomainId numDomains{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> freezeReq{false};
    std::atomic<bool> abandon{false};  // on stop, leak the Peer instead of running its dtor
    std::atomic<std::uint64_t> acqCount{0};
};

void worker(PeerSlot_t* slot)
{
    slot->peer = std::make_unique<cme::Peer>(*slot->region, slot->peerId, slot->coherency);
    for (cme::DomainId domainId = 1; domainId <= slot->numDomains; ++domainId)
    {
        slot->peer->joinDomain(domainId);
    }
    cme::DomainId domainId = 1;
    while (!slot->stop.load(std::memory_order_acquire))
    {
        slot->peer->setFreeze(slot->freezeReq.load(std::memory_order_acquire));
        if (slot->freezeReq.load(std::memory_order_acquire))
        {
            harness::sleepMs(20);
            continue;
        }
        try
        {
            auto guard = slot->peer->lock(domainId);
            slot->acqCount.fetch_add(1, std::memory_order_relaxed);
        }
        catch (const cme::LockTimeoutError&)
        {
            // @expected a lock attempt that times out under contention is a normal outcome, not a failure: the loop moves on to the next domain.
        }
        domainId = (domainId % slot->numDomains) + 1;
    }
    if (slot->abandon.load(std::memory_order_acquire))
    {
        static_cast<void>(slot->peer.release());  // crashed peer: never run its dtor on the recovered slot
    }
    else
    {
        slot->peer.reset();  // clean leave + destroy
    }
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const cme::Strategy strategy = ctx.strategy();
    const char* const stratSuffix = ctx.strategySuffix();

    constexpr cme::PeerId MaxPeers = 4;
    constexpr cme::DomainId NumDomains = 2;
    constexpr cme::DomainId Ceiling = NumDomains + 1;  // control + data
    constexpr cme::PeerId Dead = 1;                    // stranded target (has survivors either side)

    std::optional<cme::Geometry> region;
    const cme::Geometry::FormatOpts_t fmtOpts{strategy};
    region.emplace(ctx.memory().createRegion(Ceiling, MaxPeers, fmtOpts));
    harness::seedDataDomains(*region, NumDomains, ctx.coherency());

    std::printf("recovery resume: %u peers (%s, backend=%s)\n", MaxPeers, stratSuffix,
                ctx.backendName());

    std::array<PeerSlot_t, MaxPeers> peers{};
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        peers[i].region = &*region;
        peers[i].peerId = i;
        peers[i].numDomains = NumDomains;
        peers[i].coherency = ctx.coherency();
        peers[i].tid = std::thread{worker, &peers[i]};
    }
    harness::sleepMs(1000);  // memberships go Active; ownership spreads

    auto deadStatusIs = [&](Status status)
    {
        return cme::coherency::get(region->getMemberSlot(Dead), ctx.coherency()).hasStatus(status);
    };
    ctx.check(deadStatusIs(Status::Active), "dead slot Active before crash");

    // Flip the crashed target to Recovering with its claim slot left NoPeer -- the residue
    // of an RA that died before committing None. Inside the grace window, so no survivor
    // has entered the normal path yet.
    peers[Dead].freezeReq.store(true);
    harness::sleepMs(150);  // worker applies setFreeze; poll thread stops stamping
    cme::coherency::rmwIfTrue(region->getMemberSlot(Dead), ctx.coherency(),
                              [](auto* member)
                              {
                                  if (!member->isValidMagic())
                                  {
                                      return false;
                                  }
                                  member->setStatus(Status::Recovering);
                                  return true;
                              });
    ctx.check(deadStatusIs(Status::Recovering), "target stranded in Recovering (RA presumed dead)");

    // A surviving RA must detect the strand and drive the slot to None. Pre-fix, the ring
    // walk never re-selects a Recovering peer, so this never happens and the wait times out.
    const bool becameNone = harness::waitUntil([&]
                                               {
                                                   return deadStatusIs(Status::None);
                                               },
                                               RecoveryDeadlineMs);
    ctx.check(becameNone, "surviving RA resumed recovery -> stranded slot reached None");

    // Survivors kept making progress across the resume (domains stayed usable).
    std::array<std::uint64_t, MaxPeers> pre{};
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        pre[i] = peers[i].acqCount.load();
    }
    harness::sleepMs(500);
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        if (i == Dead)
        {
            continue;
        }
        ctx.check(peers[i].acqCount.load() > pre[i], "survivor advanced after resume");
    }

    // Teardown. The crashed peer never rejoins: its worker leaks the Peer (abandon) so
    // the dtor never touches the now-recovered slot; the rest leave cleanly.
    peers[Dead].abandon.store(true);
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        peers[i].stop.store(true);
    }
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        if (peers[i].tid.joinable())
        {
            peers[i].tid.join();
        }
    }
    region.reset();
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
