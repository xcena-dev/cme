// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// guard_probe.cpp -- CmedGuard as a value: moved, assigned over, released by hand.
//
// A caller that locks conditionally moves the turn around, and each move has a way to go wrong
// that a plain lock and scope exit never reaches: a moved-from guard that still releases, an
// assignment that drops the destination's turn, an explicit unlock that runs twice.
//
// The stub counts what it is asked to revoke, so an extra release shows up as a number.

#include <cinttypes>
#include <cstdint>
#include <string>
#include <utility>

#include "cmed/guard.hpp"
#include "cmed/session.hpp"
#include "harness/helper.hpp"
#include "harness/helper_requester.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

constexpr const char* FirstName = "lane0";
constexpr const char* SecondName = "lane1";

constexpr std::uint32_t FirstSlot = cmed::harness::FirstDataSlot;
constexpr std::uint32_t SecondSlot = FirstSlot + 1;

// One revoke is owed per acquire the cases below make.
constexpr std::uint32_t ExpectedGrants = 5;

// How those acquires divide between the two domains: every case names the first one, and the move
// assignment is the only one that also names the second.
constexpr std::uint32_t FirstSlotAcquires = 4;
constexpr std::uint32_t SecondSlotAcquires = 1;

// A revoke still pending when the next acquire rings the same domain merges with it into one bit,
// and the daemon then sees the request alone. Each case drains its own revokes before returning.
void settleRevokes(const cmed::harness::StubDaemon& daemon, std::uint32_t before, std::uint32_t owed)
{
    static_cast<void>(daemon.awaitReleases(before + owed));
}

// The source must end the move holding nothing. A source that still released at its own scope exit
// would end the span the destination is inside, and the next requester would enter it.
void moveConstructionCarriesTheTurn(probe::Context& ctx,
                                    cmed::CmedSession& session,
                                    cmed::protocol::SharedArea_t& area,
                                    const cmed::harness::StubDaemon& daemon)
{
    ctx.openCase("move construction");
    const std::uint32_t before = daemon.releases();
    {
        cmed::CmedGuard source = session.lock(FirstName);
        const cmed::CmedGuard destination{std::move(source)};

        // NOLINTNEXTLINE(bugprone-use-after-move): reading the moved-from guard is the assertion.
        ctx.check(!source, "the moved-from guard holds nothing");
        ctx.check(static_cast<bool>(destination), "the destination holds");
        ctx.check(cmed::harness::isHeld(area, FirstSlot), "and the domain is still held");
    }
    ctx.check(cmed::harness::isIdle(area, FirstSlot), "one scope exit returns it to Idle");
    settleRevokes(daemon, before, 1);
}

// Assignment onto a guard that already holds one has to give that turn back first. Leaving it held
// would strand the domain: no guard would be left pointing at it.
void moveAssignmentGivesTheDestinationsTurnBack(probe::Context& ctx,
                                                cmed::CmedSession& session,
                                                cmed::protocol::SharedArea_t& area,
                                                const cmed::harness::StubDaemon& daemon)
{
    ctx.openCase("move assignment onto a guard that holds");
    const std::uint32_t before = daemon.releases();
    {
        cmed::CmedGuard first = session.lock(FirstName);
        cmed::CmedGuard second = session.lock(SecondName);
        if (!ctx.check(cmed::harness::isHeld(area, FirstSlot) && cmed::harness::isHeld(area, SecondSlot),
                       "both domains start held"))
        {
            return;
        }

        first = std::move(second);

        // NOLINTNEXTLINE(bugprone-use-after-move): reading the moved-from guard is the assertion.
        ctx.check(!second, "the source holds nothing afterwards");
        ctx.check(static_cast<bool>(first), "the destination holds");
        ctx.check(cmed::harness::isIdle(area, FirstSlot), "the destination's own turn was given back");
        ctx.check(cmed::harness::isHeld(area, SecondSlot), "and the turn it took is still held");
    }
    ctx.check(cmed::harness::isIdle(area, SecondSlot), "the scope exit returns the surviving turn");
    settleRevokes(daemon, before, 2);
}

// Through a reference, because the compiler rejects the literal `guard = std::move(guard)` and the
// guard against it is a runtime one: what is under test is the self-check inside operator=.
void selfAssignmentKeepsTheTurn(probe::Context& ctx,
                                cmed::CmedSession& session,
                                cmed::protocol::SharedArea_t& area,
                                const cmed::harness::StubDaemon& daemon)
{
    ctx.openCase("self-assignment");
    const std::uint32_t before = daemon.releases();
    {
        cmed::CmedGuard guard = session.lock(FirstName);
        cmed::CmedGuard& alias = guard;
        guard = std::move(alias);

        ctx.check(static_cast<bool>(guard), "the guard still holds");
        ctx.check(cmed::harness::isHeld(area, FirstSlot), "and so does the domain");
    }
    ctx.check(cmed::harness::isIdle(area, FirstSlot), "the scope exit returns it once");
    settleRevokes(daemon, before, 1);
}

// The second unlock must reach the daemon with nothing, and the destructor after it too. Both are
// counted rather than assumed: the release total at the end is what says so.
void anExplicitUnlockRunsOnce(probe::Context& ctx,
                              cmed::CmedSession& session,
                              cmed::protocol::SharedArea_t& area,
                              const cmed::harness::StubDaemon& daemon)
{
    ctx.openCase("an explicit unlock");

    const std::uint32_t before = daemon.releases();
    cmed::CmedGuard guard = session.lock(FirstName);
    guard.unlock();
    ctx.check(!guard, "the guard holds nothing after unlock");
    ctx.check(cmed::harness::isIdle(area, FirstSlot), "and the domain is Idle");

    // The first revoke is drained before the second unlock runs, so a revoke that unlock sends
    // arrives on a bitmap holding no bit for this domain and is counted instead of merged.
    ctx.check(daemon.awaitReleases(before + 1), "the first unlock's revoke reached the daemon");

    guard.unlock();
    ctx.check(!guard && cmed::harness::isIdle(area, FirstSlot), "a second unlock changes nothing");
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"guard-probe"};
    return probe::run(
        "guard probe",
        [&scratch](probe::Context& ctx)
        {
            const std::string areaName = scratch.makeAreaName("");
            cmed::harness::ProbeArea area{areaName.c_str()};
            cmed::harness::publishDomains(area.shared(), cmed::harness::FirstDataSlot,
                                          {FirstName, SecondName});

            const cmed::harness::StubDaemon daemon{area.shared()};
            const cmed::harness::StubSetup setup{scratch.makePath("cmed.sock"), area.descriptor()};
            cmed::CmedSession session = setup.openRequester();

            moveConstructionCarriesTheTurn(ctx, session, area.shared(), daemon);
            moveAssignmentGivesTheDestinationsTurnBack(ctx, session, area.shared(), daemon);
            selfAssignmentKeepsTheTurn(ctx, session, area.shared(), daemon);
            anExplicitUnlockRunsOnce(ctx, session, area.shared(), daemon);

            ctx.openCase("what the daemon was asked for");
            static_cast<void>(daemon.awaitGrants(ExpectedGrants));
            ctx.checkf(daemon.grants() == ExpectedGrants, "%" PRIu32 " grants", daemon.grants());
            ctx.checkf(daemon.grantsFor(FirstSlot) == FirstSlotAcquires &&
                           daemon.grantsFor(SecondSlot) == SecondSlotAcquires,
                       "%" PRIu32 " of them on the first domain and %" PRIu32 " on the second",
                       daemon.grantsFor(FirstSlot), daemon.grantsFor(SecondSlot));

            // The count that makes a moved-from guard's release visible: a turn given back twice
            // would ask for two revokes, and nothing in the state words afterwards would say so.
            static_cast<void>(daemon.awaitReleases(ExpectedGrants));
            ctx.checkf(daemon.releases() == ExpectedGrants, "%" PRIu32 " revokes, one per grant",
                       daemon.releases());

            // Per domain, because a guard revoking the first domain twice and the second never asks
            // for the same five revokes as one revoking each domain it held.
            ctx.checkf(daemon.releasesFor(FirstSlot) == FirstSlotAcquires,
                       "%" PRIu32 " revokes on the first domain", daemon.releasesFor(FirstSlot));
            ctx.checkf(daemon.releasesFor(SecondSlot) == SecondSlotAcquires,
                       "%" PRIu32 " revokes on the second domain", daemon.releasesFor(SecondSlot));
        });
}
