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

}  // namespace

void runBody(harness::TestContext& ctx)
{
    constexpr cme::PeerId MaxPeers = 4;
    constexpr cme::DomainId NumDomains = 2;
    constexpr cme::DomainId Ceiling = NumDomains + 1;  // control + data
    constexpr cme::PeerId Dead = 1;                    // stranded target
    constexpr cme::PeerId LiveRA = 2;                  // peer named by the seeded claim

    auto region = harness::createRegion(Ceiling, MaxPeers);
    harness::seedDataDomains(region, NumDomains);

    std::printf("recovery stake-gate: %u peers (%s, backend=%s)\n", MaxPeers,
                ctx.strategySuffix(), ctx.backendName());

    std::array<harness::PeerSlot_t, MaxPeers> peers{};
    harness::spawnPeerWorkers(peers, MaxPeers, region, NumDomains);
    harness::sleepMs(1000);  // memberships go Active; ownership spreads
    ctx.check(harness::allPeersJoined(peers, MaxPeers), "every worker joined its domains");

    auto deadStatusIs = [&](Status status)
    {
        return harness::hasMemberStatus(region, Dead, status);
    };
    auto* claimSlot = cme::RecoveryAuthorityLayout{region}.getClaim(Dead);
    auto claimRA = [&]() -> cme::PeerId
    {
        return static_cast<cme::PeerId>(cme::coherency::get(claimSlot, ctx.coherency()).recoveryAuthority);
    };

    ctx.check(deadStatusIs(Status::Active), "dead slot Active before crash");

    // Construct the "live claim in progress" residue: crash the target (leave it Active,
    // heartbeat stalled) and stamp its claim word with a LIVE RA (LiveRA). Seeded inside
    // the liveness grace window, before hasFailed(Dead) trips and peer 0 reaches claim().
    peers[Dead].frozen.store(true);
    harness::sleepMs(150);
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
        harness::holdsFor([&]
                          {
                              return claimRA() == LiveRA && deadStatusIs(Status::Active);
                          },
                          4000);
    ctx.check(wordHeld, "live claim not overwritten; slot not taken over while RA alive");

    // Phase B: kill the recorded RA. Its word now names a dead peer -> a surviving RA
    // stakes over it (branch (b)) and drives the stranded slot to None.
    peers[LiveRA].frozen.store(true);
    const bool becameNone = harness::waitUntil([&]
                                               {
                                                   return deadStatusIs(Status::None);
                                               },
                                               RecoveryDeadlineMs);
    ctx.check(becameNone, "dead-RA claim stolen -> stranded slot reached None");

    // Survivors (not the two frozen peers) kept making progress.
    std::array<std::uint64_t, MaxPeers> pre{};
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        pre[i] = peers[i].acquires.load();
    }
    harness::sleepMs(500);
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        if (i == Dead || i == LiveRA)
        {
            continue;
        }
        ctx.check(peers[i].acquires.load() > pre[i], "survivor advanced after resume");
    }

    // Teardown. The two frozen peers never rejoin: leak their Peer so no dtor touches the
    // recovered slots; the rest leave cleanly.
    peers[Dead].abandon.store(true);
    peers[LiveRA].abandon.store(true);
    harness::joinPeerWorkers(peers, MaxPeers);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
