// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_recovery_stake_gate.cpp -- a contender stakes over a claim word ONLY when the
// recovery right is up for grabs. A live claim is left alone; a dead RA's word is stolen.
//
// White-box: the real race is tens of us, too tight to hit by wall clock. Dead is kept
// Active-but-dead so its RA actually reaches claim() -- a Recovering target is gated out
// before that -- and ring order makes peer 0 its sole RA.
//   Phase A: seed Dead's claim word to name the live LiveRA. Peer 0 must yield, leaving
//            the slot Active and the word unchanged across the window.
//   Phase B: freeze LiveRA too. Peer 0 must now steal the word and drive Dead to None,
//            which a too-strict gate would strand.
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
#include "core/policy/recovery_authority_layout.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace
{

using Status = cme::Geometry::Member_t::Status;

constexpr std::uint32_t RecoveryDeadlineMs = 15000;

// Holds true for the whole window (no rival wrote the word / drove the slot).
template <typename T>
bool holdsFor(T pred, std::uint32_t windowMs)
{
    for (std::uint32_t waited = 0; waited < windowMs; waited += PollStepMs)
    {
        if (!pred())
        {
            return false;
        }
        sleepMs(PollStepMs);
    }
    return pred();
}

// Minimal lock-loop worker (mirrors test_recovery_resume): joins the data domains and
// keeps acquiring so ownership spreads and a frozen peer likely holds a domain at crash.
struct PeerSlot_t
{
    std::thread tid;
    std::unique_ptr<cme::Peer> peer;
    cme::Geometry* region{nullptr};
    cme::PeerId peerId{0};
    cme::CoherencyMode coherency{};
    cme::DomainId numDomains{0};
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
            sleepMs(20);
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

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const cme::Strategy strategy = ctx.strategy();
    const char* const stratSuffix = ctx.strategySuffix();

    constexpr cme::PeerId MaxPeers = 4;
    constexpr cme::DomainId NumDomains = 2;
    constexpr cme::DomainId Ceiling = NumDomains + 1;  // control + data
    constexpr cme::PeerId Dead = 1;                    // stranded target
    constexpr cme::PeerId LiveRA = 2;                  // peer named by the seeded claim

    std::optional<cme::Geometry> region;
    const cme::Geometry::FormatOpts_t fmtOpts{strategy};
    region.emplace(ctx.memory().createRegion(Ceiling, MaxPeers, fmtOpts));
    seedDataDomains(*region, NumDomains, ctx.coherency());

    std::printf("recovery stake-gate: %u peers (%s, backend=%s)\n", MaxPeers, stratSuffix,
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
    sleepMs(1000);  // memberships go Active; ownership spreads

    auto deadStatusIs = [&](Status status)
    {
        return cme::coherency::get(region->getMemberSlot(Dead), ctx.coherency()).hasStatus(status);
    };
    auto* claimSlot = cme::RecoveryAuthorityLayout{*region}.getClaim(Dead);
    auto claimRA = [&]() -> cme::PeerId
    {
        return static_cast<cme::PeerId>(cme::coherency::get(claimSlot, ctx.coherency()).recoveryAuthority);
    };

    ctx.check(deadStatusIs(Status::Active), "dead slot Active before crash");

    // Construct the "live claim in progress" residue: crash the target (leave it Active,
    // heartbeat stalled) and stamp its claim word with a LIVE RA (LiveRA). Seeded inside
    // the liveness grace window, before hasFailed(Dead) trips and peer 0 reaches claim().
    peers[Dead].freezeReq.store(true);
    sleepMs(150);
    cme::coherency::rmwIfTrue(claimSlot, ctx.coherency(),
                              [](auto* claim)
                              {
                                  if (!claim->isValidMagic())
                                  {
                                      return false;
                                  }
                                  claim->recoveryAuthority = LiveRA;
                                  return true;
                              });
    ctx.check(deadStatusIs(Status::Active), "target still Active (crashed, heartbeat stalled)");
    ctx.check(claimRA() == LiveRA, "claim word names the live RA");

    // Once hasFailed(Dead) trips, peer 0 must yield to the live claim: the word stays
    // LiveRA and the slot Active well past the grace plus a normal takeover span.
    const bool wordHeld =
        holdsFor([&]
                 {
                     return claimRA() == LiveRA && deadStatusIs(Status::Active);
                 },
                 4000);
    ctx.check(wordHeld, "live claim not overwritten; slot not taken over while RA alive");

    // Phase B: kill the recorded RA. Its word now names a dead peer -> a surviving RA
    // stakes over it (branch (b)) and drives the stranded slot to None.
    peers[LiveRA].freezeReq.store(true);
    const bool becameNone = waitUntil([&]
                                      {
                                          return deadStatusIs(Status::None);
                                      },
                                      RecoveryDeadlineMs);
    ctx.check(becameNone, "dead-RA claim stolen -> stranded slot reached None");

    // Survivors (not the two frozen peers) kept making progress.
    std::array<std::uint64_t, MaxPeers> pre{};
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        pre[i] = peers[i].acqCount.load();
    }
    sleepMs(500);
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        if (i == Dead || i == LiveRA)
        {
            continue;
        }
        ctx.check(peers[i].acqCount.load() > pre[i], "survivor advanced after resume");
    }

    // Teardown. The two frozen peers never rejoin: leak their Peer so no dtor touches the
    // recovered slots; the rest leave cleanly.
    peers[Dead].abandon.store(true);
    peers[LiveRA].abandon.store(true);
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

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, runBody);
}
