// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_readmit_gate.cpp -- a dead peer's slot is re-claimable ONLY after recovery
// marks it None (C1: the forced-fencing readmission gate).
//
// reserveMemberSlot only reuses a None slot, and a crashed peer's slot stays Active until
// recovery finalises it. A claim that races recovery must not grab the dead slot first, or
// two incarnations share it (ABA / split-brain).
//
// Deterministic setup: fill EVERY slot, then freeze one peer. Frozen != None, so:
//   - pre-recovery: 0 None slots -> claimPeerSlot throws NoFreeSlot (gate holds)
//   - post-recovery: the dead slot is None -> claim succeeds and reuses exactly it
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.
// --strategy selects the strategy.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>

#include "admission/claim.hpp"
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

// Recovery is wall-clock-timed (liveness grace + takeover + orphan sweep); poll the
// post-condition to a generous deadline rather than sleep-then-assert once.
constexpr std::uint32_t RecoveryDeadlineMs = 20000;

}  // namespace

void runBody(harness::TestContext& ctx)
{
    using Status = cme::Geometry::Member_t::Status;

    const cme::Strategy strategy = ctx.strategy();
    const char* const stratSuffix = ctx.strategySuffix();

    constexpr cme::PeerId MaxPeers = 4;   // every slot filled below
    constexpr cme::DomainId Ceiling = 3;  // control + 2 data (unused; just room)

    std::optional<cme::Geometry> region;
    const cme::Geometry::FormatOpts_t fmtOpts{strategy};
    region.emplace(ctx.memory().createRegion(Ceiling, MaxPeers, fmtOpts));

    std::printf("readmit gate: %u peers (%s, backend=%s)\n", MaxPeers, stratSuffix,
                ctx.backendName());

    // Fill every slot: no None remains anywhere.
    std::array<std::unique_ptr<cme::Peer>, MaxPeers> peers{};
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        peers[i] = std::make_unique<cme::Peer>(*region, i, ctx.coherency());
    }
    harness::sleepMs(500);  // memberships go Active; poll threads running

    constexpr cme::PeerId Dead = MaxPeers - 1;
    auto deadStatusIs = [&](Status status)
    {
        return cme::coherency::get(region->getMemberSlot(Dead), ctx.coherency()).hasStatus(status);
    };

    ctx.check(deadStatusIs(Status::Active), "dead slot Active before freeze");

    // Crash the dead peer: heartbeat stops, but the slot stays Active until recovery.
    peers[Dead]->setFreeze(true);

    // ── Phase 1: gate holds. Slot still Active (recovery not done) => 0 None slots
    // => a fresh claim finds nothing to reuse and must throw. Done promptly, inside
    // the liveness grace window. ──
    ctx.check(deadStatusIs(Status::Active), "dead slot still Active right after freeze");
    bool rejected = false;
    try
    {
        (void)cme::admission::claimPeerSlot(*region, ctx.coherency());
    }
    catch (const cme::NoFreeSlotError&)
    {
        rejected = true;
    }
    ctx.check(rejected, "pre-recovery claim rejected (no None slot to reuse)");

    // ── Phase 2: recovery flips the dead slot to None; only then is it re-claimable,
    // and the reused slot must be exactly the dead one. ──
    const bool becameNone = harness::waitUntil([&]
                                               {
                                                   return deadStatusIs(Status::None);
                                               },
                                               RecoveryDeadlineMs);
    ctx.check(becameNone, "recovery marked the dead slot None");

    cme::PeerId reclaimed = cme::NoPeer;
    bool reclaimOk = false;
    try
    {
        reclaimed = cme::admission::claimPeerSlot(*region, ctx.coherency());
        reclaimOk = true;
    }
    catch (const std::exception& e)
    {
        std::printf("  post-recovery claim threw: %s\n", e.what());
    }
    ctx.check(reclaimOk, "post-recovery claim succeeded");
    ctx.check(reclaimed == Dead, "claim reused exactly the recovered slot");

    // The dead peer never rejoins (crash model): drop it without running its
    // destructor, which would otherwise touch the now-reclaimed slot.
    static_cast<void>(peers[Dead].release());
    for (cme::PeerId i = 0; i < Dead; ++i)
    {
        peers[i].reset();
    }
    region.reset();
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
