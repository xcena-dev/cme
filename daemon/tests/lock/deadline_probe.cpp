// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// deadline_probe.cpp -- what a requester does when the daemon never answers.
//
// A deadline is not a cancel. tryLock builds its guard before it judges the answer, so the release
// exchange runs on the way out even though the acquire never settled, and the domain is left at
// Idle with its mutex handed back. Nothing in a run where the daemon grants exercises that, so the
// stub here takes the request and refuses to move it.

#include <cinttypes>
#include <cstdint>
#include <optional>
#include <string>

#include "cmed/guard.hpp"
#include "cmed/session.hpp"
#include "common/timing.hpp"
#include "harness/helper.hpp"
#include "harness/helper_requester.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

constexpr const char* DomainName = "lane0";

constexpr std::uint32_t DomainSlot = cmed::harness::FirstDataSlot;

constexpr timing::Millis WaitBudget{60};

// What the caller is owed when nothing answers: the deadline ends the wait, the domain is back
// where the next requester expects it, and the mutex is not this caller's any more.
void theDeadlineEndsTheWait(probe::Context& ctx,
                            cmed::harness::ProbeArea& area,
                            const cmed::harness::StubSetup& setup,
                            const cmed::harness::StubDaemon& daemon)
{
    ctx.openCase("tryLock against a daemon that never grants");

    cmed::CmedSession session = setup.openRequester();
    cmed::protocol::Domain_t& context = cmed::harness::resolveSlot(area.shared(), DomainSlot);

    const timing::Stopwatch waited;
    const std::optional<cmed::CmedGuard> guard = session.tryLock(DomainName, WaitBudget);

    ctx.check(!guard.has_value(), "tryLock answers nullopt");
    ctx.check(waited.elapsed() >= WaitBudget, "and it waited out the deadline it was given");
    ctx.check(daemon.grants() == 0, "nothing was granted behind its back");
    ctx.check(cmed::harness::isIdle(area.shared(), DomainSlot), "the domain is back at Idle");
    ctx.check(!context.hasWaiters(), "the queued waiter left");
    // Asked without blocking, so a mutex the deadline path leaked is a red probe and not a hang.
    ctx.check(cmed::harness::isMutexFree(context.request.lock), "and the mutex was handed back");

    // The revoke the caller never asked for. It is what keeps a grant landing after the deadline
    // from being left to whoever takes the domain next.
    static_cast<void>(daemon.awaitReleases(1));
    ctx.checkf(daemon.releases() == 1, "the release exchange ran anyway (%" PRIu32 " revoke)",
               daemon.releases());
}

// The domain has to be usable afterwards, not merely tidy. A deadline that left the state machine
// or the mutex mid-exchange would show up here and nowhere earlier.
void aLaterDaemonStillGrantsIt(probe::Context& ctx, cmed::harness::ProbeArea& area,
                               const cmed::harness::StubSetup& setup)
{
    ctx.openCase("the same domain, once a daemon answers");

    const cmed::harness::StubDaemon daemon{area.shared()};
    cmed::CmedSession session = setup.openRequester();
    {
        const cmed::CmedGuard guard = session.lock(DomainName);
        ctx.check(static_cast<bool>(guard), "the next requester locks it");
        ctx.check(cmed::harness::isHeld(area.shared(), DomainSlot), "and the domain says so");
    }

    static_cast<void>(daemon.awaitGrants(1));
    ctx.check(daemon.grants() == 1, "one grant, from the daemon that answers");
    ctx.check(cmed::harness::isIdle(area.shared(), DomainSlot), "and the release returns it to Idle");
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"deadline-probe"};
    return probe::run(
        "deadline probe",
        [&scratch](probe::Context& ctx)
        {
            const std::string areaName = scratch.makeAreaName("");
            cmed::harness::ProbeArea area{areaName.c_str()};
            cmed::harness::publishDomain(area.shared(), DomainSlot, DomainName);
            const cmed::harness::StubSetup setup{scratch.makePath("cmed.sock"), area.descriptor()};

            {
                const cmed::harness::StubDaemon silent{
                    area.shared(), cmed::harness::answerNoLock()};
                theDeadlineEndsTheWait(ctx, area, setup, silent);
            }

            aLaterDaemonStillGrantsIt(ctx, area, setup);
        });
}
