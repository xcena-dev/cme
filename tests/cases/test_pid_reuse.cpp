// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_pid_reuse.cpp -- the next occupant of a recovered slot inherits nothing.
//
// slot_inherit checks the same claim for a peer that left cleanly, where leaveMembership clears
// participation in the write that sets None. The dead route reaches None through recovery instead,
// and readmit_gate stops at the gate rather than asking what the new occupant then holds.
//
// Two domains, so a partial clear reads as one surviving bit rather than as nothing happening.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "cme/errors.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

constexpr cme::PeerId MaxPeers = 4;
constexpr cme::DomainId NumDomains = 2;
constexpr cme::DomainId Ceiling = NumDomains + 1;  // control + data
constexpr cme::PeerId Dead = 1;                    // the incarnation that crashes
constexpr cme::DomainId First = 1;
constexpr cme::DomainId Second = 2;

constexpr std::uint32_t RecoveryDeadlineMs = 20000;

}  // namespace

void runBody(harness::TestContext& ctx)
{
    auto region = harness::createRegion(Ceiling, MaxPeers);
    harness::seedDataDomains(region, NumDomains);

    std::printf("pid reuse: slot %u, %u domains (%s, backend=%s)\n", Dead, NumDomains,
                ctx.strategySuffix(), ctx.backendName());

    std::array<harness::PeerSlot_t, MaxPeers> peers{};
    harness::spawnPeerWorkers(peers, MaxPeers, region, NumDomains);
    harness::sleepMs(1000);  // memberships go Active; ownership spreads
    if (!ctx.check(harness::allPeersJoined(peers, MaxPeers), "every worker joined its domains"))
    {
        harness::joinPeerWorkers(peers, MaxPeers);
        return;
    }
    ctx.check(harness::participatesIn(region, Dead, First) &&
                  harness::participatesIn(region, Dead, Second),
              "the first incarnation participates in both domains");

    peers[Dead].frozen.store(true);
    peers[Dead].abandon.store(true);

    const bool freed = harness::waitUntil(
        [&region]
        {
            return harness::hasMemberStatus(region, Dead, Status::None);
        },
        RecoveryDeadlineMs);
    if (!ctx.check(freed, "recovery freed the slot"))
    {
        harness::joinPeerWorkers(peers, MaxPeers);
        return;
    }

    // The same id, a new incarnation. Nothing it did not ask for may carry over.
    auto reborn = harness::makePeer(region, Dead);
    ctx.check(!harness::participatesIn(region, Dead, First) &&
                  !harness::participatesIn(region, Dead, Second),
              "the new occupant participates in neither domain");

    const auto lockUnjoined = [&reborn]
    {
        const auto guard = reborn.lock(First);
    };
    ctx.check(harness::threw<cme::NotParticipatingError>(lockUnjoined),
              "and can be granted nothing until it joins");

    reborn.joinDomain(First);
    ctx.check(harness::participatesIn(region, Dead, First),
              "joining is what gives it the domain back");

    harness::joinPeerWorkers(peers, MaxPeers);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
