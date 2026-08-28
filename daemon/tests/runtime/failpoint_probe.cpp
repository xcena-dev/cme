// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// failpoint_probe.cpp -- what a requester is left with when its daemon dies between two writes.
//
// crash_probe kills a daemon from outside and asks what a survivor makes of the state. This arms a
// named boundary instead, so the death lands in the gap a case names rather than wherever the signal
// happened to arrive. The boundaries are daemon/observe/failpoint.hpp's.
//
// Skipped whole without CMED_FAILPOINT: an unarmed daemon serves normally, and every check below
// would then pass for want of the death it was waiting for.

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>

#include "cmed/errors.hpp"
#include "cmed/guard.hpp"
#include "cmed/session.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "daemon/observe/failpoint.hpp"
#include "harness/helper.hpp"
#include "harness/helper_cme_region.hpp"
#include "harness/helper_medium.hpp"
#include "harness/helper_scratch.hpp"
#include "harness/helper_served_daemon.hpp"
#include "test_options.hpp"

namespace
{

constexpr const char* AreaName = "failpoint";
constexpr const char* DomainName = "guarded0";

// The region this probe formats. One domain plus the control record.
constexpr std::uint32_t RegionSlots = 4;

// This probe's own session and one slot per case: a daemon killed at a boundary leaves its slot
// behind, so the next case's daemon needs one of its own rather than the one that never came back.
constexpr std::uint32_t RegionPeers = 6;

// A daemon armed at a boundary dies on the way to it, so this is how long a case waits for that.
constexpr timing::Millis DeathWait{5000};

// What a requester allows for a handshake and for a turn. Both short: every case here expects the
// daemon to die rather than to answer, so what these bound is how long a failure takes to report.
constexpr timing::Millis ReachWait{2000};
constexpr timing::Millis LockWait{2000};

// A ceiling, not an expectation: the region promises its own dead window plus a settle, and
// neither is this probe's to pick.
constexpr timing::Millis ReclaimWait{8000};

// A requester already in the domain, for the cases whose subject is what happens after that. The one
// case whose subject is the handshake itself connects on its own.
[[nodiscard]] cmed::CmedSession joinedRequester(const cmed::harness::ProbeScratch& scratch)
{
    auto session = cmed::CmedSession::connect(
        cmed::harness::clientConfigFor(scratch, AreaName, ReachWait, LockWait));
    session.joinDomain(DomainName);
    return session;
}

// The daemon dies before the Welcome goes out, so a requester never gets the area at all. What it must
// not do is wait past its own setup deadline for a handshake nobody will answer.
void aDaemonDyingBeforeTheWelcome(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a daemon that dies before its Welcome");

    cmed::harness::ArmedDaemon serving{scratch, AreaName,
                                       cmed::failpoint::readName(cmed::failpoint::Boundary::WelcomeBeforeAnswer)};
    ctx.check(serving.serving(), "the daemon started with the boundary armed and bound its socket");

    bool refused = false;
    try
    {
        auto session = cmed::CmedSession::connect(
            cmed::harness::clientConfigFor(scratch, AreaName, ReachWait, LockWait));
        static_cast<void>(session);
    }
    catch (const cmed::CmedError&)
    {
        refused = true;
    }

    ctx.check(refused, "and the requester is told rather than left waiting");
    ctx.check(serving.awaitGone(DeathWait), "the daemon left at the boundary rather than serving on");
}

// The turn is taken and the grant is not published, so the requester's slot still reads LockRequested
// when its daemon goes. Its own deadline is what has to end the wait.
void aDaemonDyingBeforeTheGrant(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a daemon that dies before it publishes a grant");

    cmed::harness::ArmedDaemon serving{scratch, AreaName,
                                       cmed::failpoint::readName(cmed::failpoint::Boundary::GrantBeforePublish)};
    ctx.check(serving.serving(), "the daemon started with the boundary armed and bound its socket");
    auto session = joinedRequester(scratch);

    const std::optional<cmed::CmedGuard> empty = session.tryLock(DomainName, timing::Nanos{LockWait});
    ctx.check(!empty, "the requester comes back with nothing rather than a turn nobody granted");
    ctx.check(serving.awaitGone(DeathWait), "and the daemon left at the boundary");
}

// The grant is published and nobody is woken. A requester asleep on the word has to leave on its own
// deadline, since the doorbell that would have moved it is never rung.
void aDaemonDyingBeforeTheWake(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a daemon that dies before it rings the requester");

    cmed::harness::ArmedDaemon serving{scratch, AreaName,
                                       cmed::failpoint::readName(cmed::failpoint::Boundary::GrantBeforeWake)};
    ctx.check(serving.serving(), "the daemon started with the boundary armed and bound its socket");
    auto session = joinedRequester(scratch);

    // The grant stands in the area unrung, so what this reads is whether the requester finds it by
    // looking rather than by being woken.
    const timing::Deadline spent{LockWait};
    const std::optional<cmed::CmedGuard> answered = session.tryLock(DomainName, timing::Nanos{LockWait});
    static_cast<void>(answered);

    ctx.check(spent.expired() || answered.has_value(),
              "the requester either took the standing grant or left on its own deadline");
    ctx.check(serving.awaitGone(DeathWait), "and the daemon left at the boundary");
}

// The requester was told the turn is going and the region still records this peer holding it. What a
// survivor needs is that nothing is left claiming the domain once the daemon's slot is reclaimed.
void aDaemonDyingBeforeItGivesTheTurnBack(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                                          const cmed::harness::MediumOptions_t& chosen)
{
    ctx.openCase("a daemon that dies before it gives the turn back");

    cmed::harness::ArmedDaemon serving{scratch, AreaName,
                                       cmed::failpoint::readName(cmed::failpoint::Boundary::DropBeforeRelease)};
    ctx.check(serving.serving(), "the daemon started with the boundary armed and bound its socket");

    {
        auto session = joinedRequester(scratch);
        const std::optional<cmed::CmedGuard> taken = session.tryLock(DomainName, timing::Nanos{LockWait});
        ctx.check(taken.has_value(), "a requester takes the turn while the daemon still serves");
    }

    ctx.check(serving.awaitGone(DeathWait), "and the daemon left at the boundary the release sits behind");

    // The name outlives the daemon: this probe's own session is in the domain, so what a case reads
    // here is that the death took no domain with it.
    ctx.check(cmed::harness::namePresent(chosen, DomainName), "the domain is still there afterwards");
}

// A peer's connection is gone and the domains it joined are not given back. No other node may then
// delete them, so what a case reads is whether the count settles once the daemon itself is gone.
void aDaemonDyingBeforeItGivesAJoinBack(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                                        const cmed::harness::MediumOptions_t& chosen)
{
    ctx.openCase("a daemon that dies before it gives a departed peer's join back");

    const std::uint32_t before = cmed::harness::countParticipants(chosen, DomainName);

    cmed::harness::ArmedDaemon serving{
        scratch, AreaName, cmed::failpoint::readName(cmed::failpoint::Boundary::DepartBeforeGiveBack)};
    ctx.check(serving.serving(), "the daemon started with the boundary armed and bound its socket");

    // Connected and gone, which is what drives the departure the boundary sits inside.
    {
        auto session = joinedRequester(scratch);
        static_cast<void>(session);
    }

    ctx.check(serving.awaitGone(DeathWait), "and the daemon left at the boundary");

    // The daemon's own peer slot is what carried the join, so its death takes the participation with
    // it rather than leaving a count nobody can retract.
    ctx.check(cmed::harness::countParticipants(chosen, DomainName) <= before + 1,
              "no node is left counted twice for a join nobody gave back");
}

// The region granted the turn and this daemon never recorded it, so nothing in it knows to give the
// turn back. What must not happen is the domain staying wedged: the region's own reclaim is what
// frees a turn whose holder is gone, and a daemon started after it has to reach the domain again.
void aDaemonDyingBeforeItRecordsTheTurn(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a daemon that dies holding a turn it never recorded");

    {
        cmed::harness::ArmedDaemon dying{
            scratch, AreaName, cmed::failpoint::readName(cmed::failpoint::Boundary::AcquireBeforeRecord)};
        ctx.check(dying.serving(), "the daemon started with the boundary armed and bound its socket");

        auto session = joinedRequester(scratch);
        const std::optional<cmed::CmedGuard> empty = session.tryLock(DomainName, timing::Nanos{LockWait});
        ctx.check(!empty, "the requester comes back with nothing rather than a turn nobody recorded");
        ctx.check(dying.awaitGone(DeathWait), "and the daemon left holding it");
    }

    // Unarmed, so this one serves. Its own socket, since the dead one left its file behind.
    static_cast<void>(::unlink(cmed::harness::daemonSocketPath(scratch, AreaName).c_str()));
    const cmed::harness::NodeDaemon serving{cmed::harness::daemonConfigPath(scratch, AreaName)};
    ctx.check(serving.started() && cmed::harness::awaitDaemonBound(scratch, AreaName, ReclaimWait),
              "a daemon started after it comes up on the same area");

    // Generous: what is waited out is the region's dead window, which is not this probe's to pick.
    const bool regained = poll::waitUntil(
        [&scratch]
        {
            try
            {
                auto session = joinedRequester(scratch);
                return session.tryLock(DomainName, timing::Nanos{LockWait}).has_value();
            }
            catch (const cmed::CmedError&)
            {
                return false;
            }
        },
        ReclaimWait, cmed::harness::DaemonStartPoll);
    ctx.check(regained, "and the domain is reachable again rather than wedged behind a turn nobody holds");
}

}  // namespace

int main()
{
    // Before anything is built: an unarmed daemon serves normally, so every case below would pass for
    // want of the death it was waiting for.
    if (!cmed::failpoint::Compiled)
    {
        std::printf("SKIP: built without CMED_FAILPOINT, so no boundary is reachable\n");
        return ::harness::SkipExitCode;
    }

    cmed::harness::ProbeScratch scratch{"failpoint-probe"};
    const std::string regionLabel = scratch.makeAreaName("region");

    // probe::run answers the way main does, so this is a code and not a verdict.
    int outcome = 1;
    try
    {
        cmed::harness::ProbeRegion region{regionLabel.c_str(), RegionSlots, RegionPeers};
        region.createDomain(DomainName);

        // Named for the readers that look at the region from outside. shm throughout: what this probe
        // varies is where the daemon dies, and a medium axis would multiply that by three for nothing.
        cmed::harness::MediumOptions_t chosen;
        chosen.uri = std::string{"shm:"} + regionLabel;

        cmed::harness::DaemonSite_t site;
        site.areaName = AreaName;
        site.uri = chosen.uri;
        site.extra = "serve:\n  idle_interval_ms: 50\nregistry:\n  refresh_interval_ms: 100\n";
        cmed::harness::writeDaemonConfig(scratch, site);

        outcome = probe::run("failpoint probe",
                             [&scratch, &chosen](probe::Context& ctx)
                             {
                                 aDaemonDyingBeforeTheWelcome(ctx, scratch);
                                 aDaemonDyingBeforeTheGrant(ctx, scratch);
                                 aDaemonDyingBeforeTheWake(ctx, scratch);
                                 aDaemonDyingBeforeItGivesTheTurnBack(ctx, scratch, chosen);
                                 aDaemonDyingBeforeItGivesAJoinBack(ctx, scratch, chosen);
                                 aDaemonDyingBeforeItRecordsTheTurn(ctx, scratch);
                             });
    }
    catch (const std::exception& failure)
    {
        std::printf("failpoint probe: %s\n", failure.what());
        return 1;
    }

    return outcome;
}
