// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_failpoint_handoff.cpp -- a holder killed while handing a domain over.
//
// TransferBeforePublish is SpecAction: the record still names the dying holder, which is the state
// the spec reaches with Crash alone, and DeadHolderPending says recovery owes the domain.
// TransferBeforeTruth is Mechanism: the shadow is ahead of the truth, and the spec has no shadow.
// shadow_authority forges that state; here a crash leaves it.

#include <cstdint>
#include <cstdio>
#include <exception>

#include "cme/shared.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "observe/failpoint.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

// The two boundaries this file is about.
constexpr auto BeforePublish = cme::failpoint::Boundary::TransferBeforePublish;
constexpr auto BeforeTruth = cme::failpoint::Boundary::TransferBeforeTruth;

constexpr std::uint32_t MaxDomains = 2;
constexpr cme::PeerId MaxPeers = 3;
constexpr cme::DomainId Contested = 1;
constexpr const char* ContestedName = "lane0";
constexpr std::uint32_t RecoveryDeadlineMs = 20000;

// Cheap enough to repeat: each round costs one recovery, not one grace window per peer.
constexpr std::uint32_t Rounds = 3;

// What one round observed. `crashed` is what tells a verdict from an accident: a round that never
// reached the boundary saw a clean departure, and its recovery says nothing about this boundary.
struct RoundResult_t
{
    bool crashed;
    bool shadowAhead;
    bool cameBack;
};

// Whether any group's shadow sits above its truth, on any domain. publishDomainRecord writes the
// shadow first, so one left above the truth is a publish interrupted between the two writes.
//
// Every domain, because arm() is per process and not per domain: the holder's own poll cycle
// transfers the control domain too, and that is the publish the boundary usually lands in.
[[nodiscard]] bool anyShadowAheadOfTruth(const cme::Geometry& region)
{
    for (cme::DomainId domainId = 0; domainId < region.getDomainCount(); ++domainId)
    {
        const auto truthEpoch =
            static_cast<std::uint64_t>(harness::readDomainRecord(region, domainId).epoch);
        for (cme::PeerId peerId = 0; peerId < region.getPeerCount(); ++peerId)
        {
            const auto shadowEpoch = static_cast<std::uint64_t>(
                harness::readDomainRecordShadow(region, domainId, peerId).epoch);
            if (shadowEpoch > truthEpoch)
            {
                return true;
            }
        }
    }
    return false;
}

// One crash and the recovery after it. A fresh region each round, since the residue of one would
// decide the next.
[[nodiscard]] RoundResult_t oneRound(cme::failpoint::Boundary boundary)
{
    harness::formatSession(MaxDomains, MaxPeers);
    {
        auto seeder = harness::openSession();
        seeder.createDomain(ContestedName);
    }

    // A live peer throughout, so the region keeps a recovery authority. Participation is opt-in and
    // only the creator gets it for free, so every other session joins before it locks.
    auto survivor = harness::openSession();
    survivor.joinDomain(ContestedName);

    // The release is what runs transferOwnership, so the guard has to go out of scope in the child.
    const bool died = harness::killChildAt(
        boundary,
        []
        {
            auto session = harness::openSession();
            session.joinDomain(ContestedName);
            const auto guard = session.lock(ContestedName);
            static_cast<void>(guard);
        });
    if (!died)
    {
        // Peterson vacates on release instead of transferring, so a boundary inside
        // transferOwnership can go unreached. The peer then left cleanly and the domain comes back
        // for a reason this case is not about.
        std::printf("  the holder left without reaching %s\n",
                    cme::failpoint::readName(boundary));
    }

    // Read before the grace window runs out, so the residue is the crash's and not recovery's.
    auto region = harness::openBoundRegion();
    const cme::PeerId named = harness::readDomainRecord(region, Contested).holder;
    const bool shadowAhead = died && anyShadowAheadOfTruth(region);

    bool reclaimed = false;
    const bool got = harness::waitUntil(
        [&survivor, &reclaimed]
        {
            try
            {
                const auto guard = survivor.lock(ContestedName);
                reclaimed = true;
            }
            catch (const std::exception&)
            {
                return false;  // still held by the corpse; recovery has not run yet
            }
            return true;
        },
        RecoveryDeadlineMs);
    std::printf("  %s: record named peer %u, shadow %s the truth, domain %s\n",
                cme::failpoint::readName(boundary), named, shadowAhead ? "above" : "level with",
                got && reclaimed ? "came back" : "did not come back");
    return {died, shadowAhead, got && reclaimed};
}

// A domain nobody holds and nobody can reach is the failure both boundaries risk. Rounds, because
// what varies is not the crash but what the survivor's poll cycle was doing when the residue landed.
//
// @shadowExpected says whether this boundary sits between the shadow write and the truth write,
// which is the residue that tells the two boundaries apart.
void checkDomainComesBack(harness::TestContext& ctx, cme::failpoint::Boundary boundary,
                          bool shadowExpected, const char* what)
{
    bool cameBackEvery = true;
    std::uint32_t crashed = 0;
    std::uint32_t shadowAhead = 0;
    for (std::uint32_t round = 0; round < Rounds; ++round)
    {
        const RoundResult_t result = oneRound(boundary);
        crashed += result.crashed ? 1 : 0;
        shadowAhead += result.shadowAhead ? 1 : 0;
        cameBackEvery = result.cameBack && cameBackEvery;
    }

    // Peterson's unlock vacates the record where the others hand to a successor, so it never enters
    // transferOwnership and cannot strand a domain there. Named in the line, not in the code, so a
    // second policy going quiet shows up instead of hiding behind a hardcoded exception.
    if (crashed == 0)
    {
        ctx.checkf(cameBackEvery,
                   "no round reached %s under %s, and every clean departure left the domain "
                   "reachable",
                   cme::failpoint::readName(boundary), ctx.strategySuffix());
        return;
    }
    ctx.check(cameBackEvery, what);
    ctx.checkf(shadowAhead == (shadowExpected ? crashed : 0),
               "%s left a shadow above the truth in %u of the %u rounds that crashed there",
               cme::failpoint::readName(boundary), shadowAhead, crashed);
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so nothing would be killed");
    }

    checkDomainComesBack(ctx, BeforePublish, false,
                         "a crash before the record write leaves the domain reachable");
    checkDomainComesBack(ctx, BeforeTruth, true,
                         "a crash between the shadow and the truth leaves the domain reachable");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
