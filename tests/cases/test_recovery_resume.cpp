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
#include <optional>

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

}  // namespace

void runBody(harness::TestContext& ctx)
{
    constexpr cme::PeerId MaxPeers = 4;
    constexpr cme::DomainId NumDomains = 2;
    constexpr cme::DomainId Ceiling = NumDomains + 1;  // control + data
    constexpr cme::PeerId Dead = 1;                    // stranded target (has survivors either side)

    std::optional<cme::Geometry> region;
    region.emplace(harness::createRegion(ctx, Ceiling, MaxPeers));
    harness::seedDataDomains(*region, NumDomains, ctx.coherency());

    std::printf("recovery resume: %u peers (%s, backend=%s)\n",
                MaxPeers, ctx.strategySuffix(), ctx.backendName());

    std::array<harness::PeerSlot_t, MaxPeers> peers{};
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        harness::spawnPeerWorker(peers[i], i, *region, ctx.coherency(), NumDomains);
    }
    harness::sleepMs(1000);  // memberships go Active; ownership spreads
    ctx.check(harness::allPeersJoined(peers, MaxPeers), "every worker joined its domains");

    auto deadStatusIs = [&](Status status)
    {
        return harness::hasMemberStatus(*region, Dead, status, ctx.coherency());
    };
    ctx.check(deadStatusIs(Status::Active), "dead slot Active before crash");

    // Flip the crashed target to Recovering with its claim slot left NoPeer -- the residue
    // of an RA that died before committing None. Inside the grace window, so no survivor
    // has entered the normal path yet.
    peers[Dead].frozen.store(true);
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
        pre[i] = peers[i].acquires.load();
    }
    harness::sleepMs(500);
    for (cme::PeerId i = 0; i < MaxPeers; ++i)
    {
        if (i == Dead)
        {
            continue;
        }
        ctx.check(peers[i].acquires.load() > pre[i], "survivor advanced after resume");
    }

    // Teardown. The crashed peer never rejoins: its worker leaks the Peer (abandon) so
    // the dtor never touches the now-recovered slot; the rest leave cleanly.
    peers[Dead].abandon.store(true);
    harness::joinPeerWorkers(peers, MaxPeers);
    region.reset();
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
