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

// Whether the round judged anything, and if so what. Unreached is not a verdict: no crash happened
// there, so the recovery that follows is a clean departure's and says nothing about this boundary.
enum class RoundOutcome
{
    CameBack,
    Stranded,
    Unreached,
};

// One crash and the recovery after it. A fresh region each round, since the residue of one would
// decide the next.
[[nodiscard]] RoundOutcome oneRound(cme::failpoint::Boundary boundary)
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
        std::printf("  the holder left without reaching %s\n", cme::failpoint::nameOf(boundary));
        return RoundOutcome::Unreached;
    }

    auto region = harness::openBoundRegion();
    const cme::PeerId named = harness::readDomainRecord(region, Contested).holder;

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
    std::printf("  %s: record named peer %u, domain %s\n",
                cme::failpoint::nameOf(boundary), named,
                got && reclaimed ? "came back" : "did not come back");
    return got && reclaimed ? RoundOutcome::CameBack : RoundOutcome::Stranded;
}

// A domain nobody holds and nobody can reach is the failure both boundaries risk. Rounds, because
// what varies is not the crash but what the survivor's poll cycle was doing when the residue landed.
void checkDomainComesBack(harness::TestContext& ctx, cme::failpoint::Boundary boundary,
                          const char* what)
{
    bool cameBackEvery = true;
    std::uint32_t judged = 0;
    for (std::uint32_t round = 0; round < Rounds; ++round)
    {
        const RoundOutcome outcome = oneRound(boundary);
        if (outcome == RoundOutcome::Unreached)
        {
            continue;
        }
        ++judged;
        cameBackEvery = (outcome == RoundOutcome::CameBack) && cameBackEvery;
    }

    // Peterson's unlock vacates the record where the others hand to a successor, so it never enters
    // transferOwnership and cannot strand a domain there. Named in the line, not in the code, so a
    // second policy going quiet shows up instead of hiding behind a hardcoded exception.
    if (judged == 0)
    {
        std::printf("  %s is unreachable under %s, so this round judged nothing\n",
                    cme::failpoint::nameOf(boundary), ctx.strategySuffix());
        return;
    }
    ctx.check(cameBackEvery, what);
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so nothing would be killed");
    }

    checkDomainComesBack(ctx, BeforePublish,
                         "a crash before the record write leaves the domain reachable");
    checkDomainComesBack(ctx, BeforeTruth,
                         "a crash between the shadow and the truth leaves the domain reachable");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
