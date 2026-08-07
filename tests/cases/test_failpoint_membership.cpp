// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_failpoint_membership.cpp -- a peer killed partway into joining, and partway out of leaving.
//
// All three are SpecAction. JoinMembership and PublishNone are one atomic status write in the spec,
// and BeginLeave is the transition Crash is still enabled from, so the spec says a survivor owes
// the slot either way. What the boundaries test is that the implementation's several writes leave a
// state some survivor can still finish.

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

// The three boundaries this file is about.
constexpr auto BeforeBaseline = cme::failpoint::Boundary::JoinBeforeBaseline;
constexpr auto BeforeHandoff = cme::failpoint::Boundary::LeaveBeforeHandoff;
constexpr auto BeforeNone = cme::failpoint::Boundary::LeaveBeforeNone;

constexpr std::uint32_t MaxDomains = 2;
constexpr cme::PeerId MaxPeers = 2;
constexpr cme::PeerId Victim = 1;  // the slot admission hands the child
constexpr const char* LaneName = "lane0";
constexpr std::uint32_t RecoveryDeadlineMs = 20000;

// One recovery per round, so three fits the registered timeout with room to spare.
constexpr std::uint32_t Rounds = 3;

// A child that joins, optionally takes the domain, and dies at @boundary. Whether it dies on the
// way in or on the way out is decided by which boundary is armed, not by what it does here.
[[nodiscard]] bool killAMemberAt(cme::failpoint::Boundary boundary, bool takeDomain)
{
    return harness::killChildAt(
        boundary,
        [takeDomain]
        {
            auto session = harness::openSession();
            if (takeDomain)
            {
                session.joinDomain(LaneName);  // participation is opt-in; only the creator gets it
                const auto guard = session.lock(LaneName);
                static_cast<void>(guard);
            }
        });  // session destroyed there: the leave boundaries are inside it
}

// One crash and the recovery after it, on a region of its own: the residue of one round would
// otherwise decide the next.
struct RoundResult_t
{
    bool freed{false};       // the victim's slot reached None
    bool reacquired{false};  // and the domain it may have held came back
};

[[nodiscard]] RoundResult_t oneRound(cme::failpoint::Boundary boundary, bool takeDomain)
{
    harness::formatSession(MaxDomains, MaxPeers);
    auto watcher = harness::openSession();  // slot 0, the survivor that owes the recovery
    watcher.createDomain(LaneName);

    RoundResult_t result;
    if (!killAMemberAt(boundary, takeDomain))
    {
        std::printf("  the victim left without reaching %s\n", cme::failpoint::nameOf(boundary));
        return result;  // nothing crashed, so what follows would be about a clean departure
    }

    auto region = harness::openBoundRegion();
    result.freed = harness::waitUntil(
        [&region]
        {
            return harness::hasMemberStatus(region, Victim, Status::None);
        },
        RecoveryDeadlineMs);

    // The domain has to be usable again too, or the slot was freed while its ownership was not.
    try
    {
        const auto guard = watcher.lock(LaneName);
        result.reacquired = true;
    }
    catch (const std::exception& error)
    {
        std::printf("  the survivor could not take the domain back: %s\n", error.what());
    }

    std::printf("  %s: slot %s, domain %s\n", cme::failpoint::nameOf(boundary),
                result.freed ? "reached None" : "never reached None",
                result.reacquired ? "came back" : "did not come back");
    return result;
}

// Rounds, because what varies is not the crash but where the survivor's poll cycle was when the
// residue landed.
void checkSurvivorFinishesIt(harness::TestContext& ctx, cme::failpoint::Boundary boundary,
                             bool takeDomain, const char* what)
{
    bool freedEvery = true;
    bool reacquiredEvery = true;
    for (std::uint32_t round = 0; round < Rounds; ++round)
    {
        const RoundResult_t result = oneRound(boundary, takeDomain);
        freedEvery = result.freed && freedEvery;
        reacquiredEvery = result.reacquired && reacquiredEvery;
    }
    ctx.check(freedEvery, what);
    ctx.check(reacquiredEvery, "and the domain it may have held came back every round");
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so nothing would be killed");
    }

    checkSurvivorFinishesIt(ctx, BeforeBaseline, false,
                            "a slot that went Active before its views were synced is recovered");
    checkSurvivorFinishesIt(ctx, BeforeHandoff, true,
                            "a peer that published Leaving and died holding is recovered");
    checkSurvivorFinishesIt(ctx, BeforeNone, true,
                            "a peer that handed everything off and died before None is recovered");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
