// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_recovery_zombie_claim.cpp -- claim retraction: a recovered peer's ghost claim words
// are swept at FINISH.
//
// The regression: a claim word authored by peer A outlives A's recovery. A re-admits with
// the same id and a fresh heartbeat, so isClaimantGone(A) is false everywhere -- the target
// whose claim names A is neither treated as stranded nor stakeable, and stays Recovering
// until member slots exhaust.
//
// Scenario 1 isolates retractClaimsBy: freeze A after seeding a ghost word authored by A on
// a spare None slot G, then let a survivor recover A. word[G] must return to NoPeer before
// A's slot commits to None -- G is never selected and nothing else touches its word, so
// only the hook can have wiped it.
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.
// --strategy selects the strategy.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/policy/recovery_authority_layout.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

constexpr std::uint32_t RecoveryDeadlineMs = 15000;
constexpr std::uint32_t FineStepMs = 1;  // fast poll: catch the stake inside the 20 ms settle

// Holds true for the whole window (RA did not re-stake / drive the slot).
template <typename T>
bool holdsFor(T pred, std::uint32_t windowMs)
{
    for (std::uint32_t waited = 0; waited < windowMs; waited += harness::PollStepMs)
    {
        if (!pred())
        {
            return false;
        }
        harness::sleepMs(harness::PollStepMs);
    }
    return pred();
}

// Minimal lock-loop worker (mirrors test_recovery_resume). Frozen sleep is short so a thaw
// applies fast -- scenario 2 must revive the target inside the 20 ms settle window.
struct PeerSlot_t
{
    std::thread tid;
    std::unique_ptr<cme::Peer> peer;
    cme::Geometry* region{nullptr};
    cme::PeerId peerId{0};
    cme::DomainId numDomains{0};
    cme::CoherencyMode coherency{};
    std::atomic<bool> stop{false};
    std::atomic<bool> freezeReq{false};
    std::atomic<bool> abandon{false};  // on stop, leak the Peer (don't run its dtor)
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
            harness::sleepMs(2);  // short: a thaw takes effect within ~2 ms (< settle window)
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
        static_cast<void>(slot->peer.release());  // crashed peer: never run its dtor on a recovered slot
    }
    else
    {
        slot->peer.reset();
    }
}

// Spawn peers [0, count); leave [count, maxPeers) unadmitted (their slots stay None).
void spawnPeers(std::array<PeerSlot_t, 5>& peers, cme::PeerId count, cme::Geometry& region,
                cme::CoherencyMode coherency,
                cme::DomainId numDomains)
{
    for (cme::PeerId i = 0; i < count; ++i)
    {
        peers[i].region = &region;
        peers[i].peerId = i;
        peers[i].numDomains = numDomains;
        peers[i].coherency = coherency;
        peers[i].tid = std::thread{worker, &peers[i]};
    }
}

void joinPeers(std::array<PeerSlot_t, 5>& peers, cme::PeerId count)
{
    for (cme::PeerId i = 0; i < count; ++i)
    {
        peers[i].stop.store(true);
    }
    for (cme::PeerId i = 0; i < count; ++i)
    {
        if (peers[i].tid.joinable())
        {
            peers[i].tid.join();
        }
    }
}

// ── Scenario 1: dead-RA hook sweeps a recovered peer's ghost claim words ─────
void runDeadRaHook(harness::TestContext& ctx, const std::string& uri,
                   cme::CoherencyMode coherency,
                   const cme::Geometry::FormatOpts_t& fmtOpts,
                   const std::string& stratSuffix)
{
    constexpr cme::PeerId MaxPeers = 5;
    constexpr cme::DomainId NumDomains = 2;
    constexpr cme::DomainId Ceiling = NumDomains + 1;
    constexpr cme::PeerId Workers = 4;  // peers 0..3 admitted; peer 4 stays None
    constexpr cme::PeerId DeadRa = 1;   // the RA that "died mid-recovery" and is recovered here
    constexpr cme::PeerId Ghost = 4;    // never-admitted slot hosting DeadRa's ghost claim word

    std::optional<cme::Geometry> region{ctx.memory().createRegion(Ceiling, MaxPeers, fmtOpts)};
    harness::seedDataDomains(*region, NumDomains, coherency);
    std::printf("zombie/dead-RA hook: %u peers (%s)\n", MaxPeers, stratSuffix.c_str());

    std::array<PeerSlot_t, 5> peers{};
    spawnPeers(peers, Workers, *region, coherency, NumDomains);
    harness::sleepMs(1000);  // memberships go Active; ownership spreads

    auto raStatusIs = [&](Status status)
    {
        return cme::coherency::get(region->getMemberSlot(DeadRa), coherency).hasStatus(status);
    };
    auto* ghostSlot = cme::RecoveryAuthorityLayout{*region}.getClaim(Ghost);
    auto ghostRa = [&]() -> cme::PeerId
    {
        return static_cast<cme::PeerId>(cme::coherency::get(ghostSlot, coherency).recoveryAuthority);
    };

    ctx.check(raStatusIs(Status::Active), "dead-RA slot Active before crash");

    // Crash DeadRa, then stamp a ghost claim it "authored" on the spare slot Ghost -- the
    // residue of an RA that staked a claim while recovering some other peer, then died.
    peers[DeadRa].freezeReq.store(true);
    harness::sleepMs(150);
    cme::coherency::rmwIfTrue(ghostSlot, coherency,
                              [](auto* claim)
                              {
                                  if (!claim->isValidMagic())
                                  {
                                      return false;
                                  }
                                  claim->recoveryAuthority = DeadRa;
                                  return true;
                              });
    ctx.check(ghostRa() == DeadRa, "ghost claim word names the dead RA");

    // retractClaimsBy sweeps every DeadRa-authored word BEFORE its slot commits to None,
    // so reaching None implies the ghost word is already retracted.
    const bool raRecovered = harness::waitUntil([&]
                                                {
                                                    return raStatusIs(Status::None);
                                                },
                                                RecoveryDeadlineMs);
    ctx.check(raRecovered, "dead RA driven to None by a survivor");
    ctx.check(ghostRa() == cme::NoPeer, "ghost claim authored by recovered peer retracted at FINISH");

    peers[DeadRa].abandon.store(true);  // frozen peer never rejoins
    joinPeers(peers, Workers);
    region.reset();
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const cme::Strategy strategy = ctx.strategy();
    const char* const stratSuffix = ctx.strategySuffix();
    const cme::Geometry::FormatOpts_t fmtOpts{strategy};

    runDeadRaHook(ctx, ctx.uri(), ctx.coherency(), fmtOpts, stratSuffix);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
