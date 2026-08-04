// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_recovery_orphan.cpp -- recovery-driven orphan reclaim and participation scrub.
// Every other recovery test seeds multi-participant domains, so no domain there ever
// loses its sole participant and the orphan-free path goes unexercised.
//
// Scenario A: a peer is sole participant and genesis holder of a domain. Freeze it; its
//   RA takes the domain over without participating, leaving an orphan (Active, 0
//   participants) that the sweep must free. Asserted by the name vanishing and the slot
//   becoming reusable.
// Scenario B: freeze the non-holder of a two-participant domain. Without the
//   participation scrub its phantom participation blocks deleteDomain forever.
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.
// --strategy selects the successor policy (order|request|request-agg|peterson).

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace
{

// Recovery is wall-clock-timed and varies under load, so poll the post-condition up to a
// generous deadline instead of sleeping once and asserting.
constexpr std::uint32_t RecoveryDeadlineMs = 20000;

}  // namespace

void runBody(harness::TestContext& ctx)
{
    startLogClock();

    const cme::Strategy strategy = ctx.strategy();
    const char* const stratSuffix = ctx.strategySuffix();

    // Slot ceiling = control(0) + 3 data slots; 5 peer slots (4 active here).
    constexpr cme::DomainId DomainCeiling = 4;
    constexpr cme::PeerId MaxPeers = 5;
    constexpr cme::PeerId ActivePeers = 4;

    std::optional<cme::Geometry> region;
    const cme::Geometry::FormatOpts_t fmtOpts{strategy};
    region.emplace(ctx.memory().createRegion(DomainCeiling, MaxPeers, fmtOpts));

    log("starting %u peers (%s, ceiling=%u, max_peers=%u, backend=%s)", ActivePeers,
        stratSuffix, DomainCeiling, MaxPeers, ctx.backendName());

    std::array<std::unique_ptr<cme::Peer>, MaxPeers> peers{};
    for (cme::PeerId i = 0; i < ActivePeers; ++i)
    {
        peers[i] = std::make_unique<cme::Peer>(*region, i, ctx.coherency());
    }
    sleepMs(500);  // memberships go Active; poll threads running

    // ── Scenario A: orphan-free ────────────────────────────────────────
    constexpr cme::PeerId SoloOwner = 3;  // sole participant + genesis holder
    log("A: peer %u creates 'solo' (sole participant + genesis holder)", SoloOwner);
    const auto soloId = peers[SoloOwner]->createDomain("solo");
    sleepMs(200);
    ctx.checkf(peers[0]->resolveDomainName("solo") != cme::NoDomain,
               "'solo' (id=%u) visible to survivor before crash", soloId);

    log("A: freeze peer %u (its sole participant crashes)", SoloOwner);
    peers[SoloOwner]->setFreeze(true);

    // RA takes over -> orphan (Active, holder=RA, 0 participants) -> poll-loop
    // sweep frees the slot.  Poll until the name disappears (resolve scans only
    // Active records, so NoDomain == the slot is Free again).
    const bool freed = waitUntil(
        [&]
        {
            return peers[0]->resolveDomainName("solo") == cme::NoDomain;
        },
        RecoveryDeadlineMs);
    ctx.checkf(freed, "'solo' orphan reclaimed after sole participant crash (slot freed)");

    // SoloOwner must NOT be thawed: a rejoin republishes its DRAM participation intent,
    // which still names the freed slot that scenario B reuses, so it would come back as a
    // phantom participant and block B's delete.

    // ── Scenario B: participation scrub unblocks delete ────────────────
    constexpr cme::PeerId SharedHolder = 0;  // genesis holder + participant
    constexpr cme::PeerId SharedJoiner = 1;  // second participant (non-holder)
    log("B: peer %u creates 'shared'; peer %u also joins", SharedHolder, SharedJoiner);
    const auto sharedId = peers[SharedHolder]->createDomain("shared");
    peers[SharedJoiner]->joinDomain(sharedId);
    sleepMs(200);
    ctx.checkf(peers[SharedHolder]->resolveDomainName("shared") != cme::NoDomain,
               "'shared' (id=%u) visible", sharedId);

    // With a second participant alive, delete is refused.
    bool refused = false;
    try
    {
        peers[SharedHolder]->deleteDomain(sharedId);
    }
    catch (const cme::NotParticipatingError&)
    {
        refused = true;
    }
    catch (const std::exception& e)
    {
        log("B: pre-scrub delete threw unexpected: %s", e.what());
    }
    ctx.checkf(refused, "deleteDomain refused while peer %u still participates", SharedJoiner);

    log("B: freeze peer %u (non-holder participant crashes)", SharedJoiner);
    peers[SharedJoiner]->setFreeze(true);

    // Retry delete until the scrub lands or the deadline elapses. Any exception means "not
    // yet": NotParticipatingError while the scrub is pending, or under ORDER a transient
    // lock failure while the token sits on the dead peer.
    const bool deleted = waitUntil(
        [&]
        {
            try
            {
                peers[SharedHolder]->deleteDomain(sharedId);
                return true;
            }
            catch (const std::exception&)
            {
                return false;
            }
        },
        RecoveryDeadlineMs);
    ctx.checkf(deleted, "deleteDomain succeeds after dead participant's participation scrubbed");
    ctx.checkf(peers[SharedHolder]->resolveDomainName("shared") == cme::NoDomain,
               "'shared' gone after delete");

    // ── teardown ───────────────────────────────────────────────────────
    log("shutdown");
    for (cme::PeerId i = 0; i < ActivePeers; ++i)
    {
        peers[i].reset();  // dtor stops poll thread + leaves membership
    }
    region.reset();
}

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, runBody);
}
