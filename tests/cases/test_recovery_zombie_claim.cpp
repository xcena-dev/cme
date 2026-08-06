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
#include <optional>
#include <string>

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

// A thaw has to take effect inside the 20 ms settle window scenario 2 measures, so the frozen
// worker idles far shorter than the default.
constexpr std::uint32_t ThawSleepMs = 2;

// Spawn peers [0, count); leave [count, maxPeers) unadmitted (their slots stay None).
void spawnPeers(std::array<harness::PeerSlot_t, 5>& peers, cme::PeerId count, cme::Geometry& region,
                cme::CoherencyMode coherency, cme::DomainId domainCount)
{
    for (cme::PeerId i = 0; i < count; ++i)
    {
        peers[i].idleMs = ThawSleepMs;
        harness::spawnPeerWorker(peers[i], i, region, coherency, domainCount);
    }
}

// ── Scenario 1: dead-RA hook sweeps a recovered peer's ghost claim words ─────
void runDeadRaHook(harness::TestContext& ctx, const std::string& uri,
                   cme::CoherencyMode coherency, const std::string& stratSuffix)
{
    constexpr cme::PeerId MaxPeers = 5;
    constexpr cme::DomainId NumDomains = 2;
    constexpr cme::DomainId Ceiling = NumDomains + 1;
    constexpr cme::PeerId Workers = 4;  // peers 0..3 admitted; peer 4 stays None
    constexpr cme::PeerId DeadRa = 1;   // the RA that "died mid-recovery" and is recovered here
    constexpr cme::PeerId Ghost = 4;    // never-admitted slot hosting DeadRa's ghost claim word

    std::optional<cme::Geometry> region{harness::createRegion(ctx, Ceiling, MaxPeers)};
    harness::seedDataDomains(*region, NumDomains, coherency);
    std::printf("zombie/dead-RA hook: %u peers (%s)\n", MaxPeers, stratSuffix.c_str());

    std::array<harness::PeerSlot_t, 5> peers{};
    spawnPeers(peers, Workers, *region, coherency, NumDomains);
    harness::sleepMs(1000);  // memberships go Active; ownership spreads

    auto raStatusIs = [&](Status status)
    {
        return harness::hasMemberStatus(*region, DeadRa, status, coherency);
    };
    auto* ghostSlot = cme::RecoveryAuthorityLayout{*region}.getClaim(Ghost);
    auto ghostRa = [&]() -> cme::PeerId
    {
        return static_cast<cme::PeerId>(cme::coherency::get(ghostSlot, coherency).recoveryAuthority);
    };

    ctx.check(raStatusIs(Status::Active), "dead-RA slot Active before crash");

    // Crash DeadRa, then stamp a ghost claim it "authored" on the spare slot Ghost -- the
    // residue of an RA that staked a claim while recovering some other peer, then died.
    peers[DeadRa].frozen.store(true);
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
    harness::joinPeerWorkers(peers, Workers);
    region.reset();
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const char* const stratSuffix = ctx.strategySuffix();

    runDeadRaHook(ctx, ctx.uri(), ctx.coherency(), stratSuffix);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
