// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_failpoint_recovery.cpp -- the recovery authority itself killed, at four points along it.
//
// Two deaths per check: a peer dies, and the peer recovering it dies too. What has to hold is that
// a third picks the recovery up. That is RecoveryTerminates for the SpecAction boundaries, and
// nothing but the implementation for the Mechanism ones.
//
// Processes, not the usual worker threads: SIGKILL takes the whole process, so an armed peer has to
// be one of its own or the case kills its own harness.

#include <cstdint>
#include <cstdio>

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

// The four boundaries this file is about, in the order recovery reaches them.
constexpr auto AfterClaim = cme::failpoint::Boundary::RecoveryAfterClaim;
constexpr auto MidTakeover = cme::failpoint::Boundary::TakeoverMidLoop;
constexpr auto BeforeScrub = cme::failpoint::Boundary::RecoveryBeforeScrub;
constexpr auto BeforeFinish = cme::failpoint::Boundary::RecoveryBeforeFinish;

constexpr std::uint32_t MaxDomains = 3;     // control + two data
constexpr cme::PeerId MaxPeers = 3;         // the doomed peer, its authority, and the next one
constexpr const char* HeldLane = "lane0";   // the one the doomed peer takes
constexpr const char* OtherLane = "lane1";  // so a takeover has a loop to stop halfway through

// Both deaths need a full grace window, one after the other.
constexpr std::uint32_t RecoveryDeadlineMs = 40000;

// How long the armed authority stays up waiting to reach its boundary. It dies there long before
// this, so the wait only bounds a run where the boundary is never reached.
constexpr std::uint32_t AuthorityLifetimeMs = 30000;

// Two, not three: a round pays a grace window twice over, once per death.
constexpr std::uint32_t Rounds = 2;

// The Active slot, or NoPeer. Called when the parent holds none and every child is reaped, so the
// one Active slot left is the peer that died in it.
[[nodiscard]] cme::PeerId findActiveSlot(cme::Geometry& region)
{
    for (cme::PeerId slot = 0; slot < MaxPeers; ++slot)
    {
        if (harness::hasMemberStatus(region, slot, Status::Active))
        {
            return slot;
        }
    }
    return cme::NoPeer;
}

// The parent seeds the region and then leaves it, so it holds no slot for the rest of the round and
// reads through openBoundRegion alone. Which peer the RA policy picks then cannot depend on it, and
// the children are sequenced so that only one candidate is ever alive to pick.
[[nodiscard]] bool oneRound(cme::failpoint::Boundary boundary)
{
    harness::formatSession(MaxDomains, MaxPeers);
    {
        auto seeder = harness::openSession();
        seeder.createDomain(HeldLane);
        seeder.createDomain(OtherLane);
    }
    auto region = harness::openBoundRegion();

    // The doomed peer needs no failpoint, but it does need to leave nothing behind it. _Exit skips
    // the destructors above this lambda, not the lambda's own, and a session that destructs departs
    // cleanly. Leaking both is what keeps the slot Active and the domain held with nobody there.
    harness::reapChildren(
        harness::spawnChildren(
            1,
            [](std::uint32_t)
            {
                auto* session = new cme::Session{harness::openSession()};
                session->joinDomain(HeldLane);  // participation is opt-in; only the creator gets it
                static_cast<void>(new cme::Guard{session->lock(HeldLane)});
            }));

    const cme::PeerId doomed = findActiveSlot(region);
    if (cme::isNoPeer(doomed))
    {
        std::printf("  the doomed peer left no Active slot; this round set up nothing\n");
        return false;
    }

    // The only live peer while it runs, so the claim cannot land anywhere else whatever the RA
    // policy is. Armed, so it dies partway through the recovery it started.
    const bool died = harness::killChildAt(
        boundary,
        []
        {
            auto session = harness::openSession();
            harness::sleepMs(AuthorityLifetimeMs);  // the boundary comes first
            static_cast<void>(session);
        });
    if (!died)
    {
        std::printf("  the authority left without reaching %s\n", cme::failpoint::nameOf(boundary));
        return false;
    }

    // The next authority, started only once the armed one is reaped: the assertion is that somebody
    // else finishes the takeover, and two of them alive at once would make which one a race. It
    // waits out the recovery it owes rather than a fixed span, so a round costs what it takes.
    harness::reapChildren(harness::spawnChildren(
        1,
        [doomed](std::uint32_t)
        {
            auto session = harness::openSession();
            auto ownRegion = harness::openBoundRegion();
            static_cast<void>(harness::waitUntil(
                [&ownRegion, doomed]
                {
                    return harness::hasMemberStatus(ownRegion, doomed, Status::None);
                },
                RecoveryDeadlineMs));
            static_cast<void>(session);
        }));

    const bool freed = harness::hasMemberStatus(region, doomed, Status::None);
    std::printf("  %s: the doomed peer's slot %s\n", cme::failpoint::nameOf(boundary),
                freed ? "reached None" : "never reached None");
    return freed;
}

// Rounds, because the two deaths land at different points of the survivor's poll cycle each time.
// Only two: a round costs a full grace window twice over, once per death.
void checkRecoveryResumes(harness::TestContext& ctx, cme::failpoint::Boundary boundary,
                          const char* what)
{
    bool resumedEvery = true;
    for (std::uint32_t round = 0; round < Rounds; ++round)
    {
        resumedEvery = oneRound(boundary) && resumedEvery;
    }
    ctx.check(resumedEvery, what);
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so nothing would be killed");
    }

    checkRecoveryResumes(ctx, AfterClaim,
                         "a claim won by a peer that then died is picked up again");
    checkRecoveryResumes(ctx, MidTakeover,
                         "a takeover that stopped halfway is finished by the next authority");
    checkRecoveryResumes(ctx, BeforeScrub,
                         "policy-private state left unscrubbed is scrubbed by the next authority");
    checkRecoveryResumes(ctx, BeforeFinish,
                         "a slot left Recovering reaches None under the next authority");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
