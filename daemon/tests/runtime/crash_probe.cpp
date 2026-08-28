// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// crash_probe.cpp -- what a requester holding a domain gets when its daemon is killed outright
// (SIGKILL, not SIGTERM), and what the next daemon finds.
//
// The exclusion case is cross-node: two daemons, each one peer, so an overlap means a local claim
// outlived the ownership behind it. Registered once per medium: --backend and --slot name one and the
// site config resolves it, while --uri names one whole and leaves it the caller's.

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cmed/config.hpp"
#include "cmed/errors.hpp"
#include "cmed/guard.hpp"
#include "cmed/session.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "harness/helper.hpp"
#include "harness/helper_cme_region.hpp"
#include "harness/helper_medium.hpp"
#include "harness/helper_scratch.hpp"
#include "harness/helper_served_daemon.hpp"
#include "test_memory.hpp"
#include "test_options.hpp"

namespace
{

// Two, because the subject needs a node whose daemon dies and a node that watches from outside it.
constexpr std::uint32_t Nodes = 2;

// Locked by node 0 and then taken by node 1 after the kill, which is where an overlap would show.
constexpr const char* HeldDomainName = "crash0";

// Joined by both nodes, so one node's daemon dying leaves a participant behind and the domain itself
// survives the kill.
constexpr const char* JoinedDomainName = "crash1";

// Joined by node 0 alone, which is what makes it an orphan the moment that node's daemon is gone.
constexpr const char* SoleDomainName = "crash2";

// The control domain takes slot zero.
constexpr std::uint32_t RegionSlots = 4;

// One per node plus this probe's own session, plus one spare per kill: a killed daemon's slot stays
// held until a live peer reclaims it, so its replacement needs somewhere else to go.
constexpr std::uint32_t RegionPeers = Nodes + 8;

// Held across the kill, longer than the region's dead window and its poll cycle, so a remote acquire
// that lands at all lands while this is still inside.
constexpr timing::Millis HoldAcrossKill{3000};

// What node 1 is given to get in. Generous: what is being observed is whether it gets in and when,
// and a deadline near the dead window would report a timeout as an absence.
constexpr timing::Millis RemoteWait{5000};

// Short, because a case measuring a release the daemon will never answer waits this out.
constexpr timing::Millis ReleaseWait{1000};
constexpr timing::Millis LockWait{5000};
constexpr timing::Millis ReachWait{5000};

// A ceiling, not an expectation: the region promises the dead window plus its own settle, and neither
// is this probe's to pick. Cases poll for the event and only fail once even this has passed.
constexpr timing::Millis ReclaimWait{8000};

// Past cohortHold, which is what dates the grant a dead requester rode, plus room for several
// maintenance ticks after it.
constexpr timing::Millis SweepWait{600};

// Far longer than this case's own polling ever runs, so only the parent's SIGKILL ends the child's
// wait rather than the child's own clock.
constexpr timing::Millis DeadSectionHold{30000};

// How long a daemon gets to leave on its own before it is killed outright.
// Past the daemon's own staleness bound, which is a few of its turns and far shorter than the region's.
constexpr timing::Millis StaleWait{500};

// The four the harness composes, with this probe's own indexing in front of them. Nodes are named by
// position here, and the shared unit is named by area, so this is where the two meet.
[[nodiscard]] std::string makeConfigPath(const cmed::harness::ProbeScratch& scratch,
                                         const std::vector<std::string>& areaNames, std::uint32_t node)
{
    return cmed::harness::daemonConfigPath(scratch, areaNames.at(node));
}

void writeConfig(const cmed::harness::ProbeScratch& scratch, const std::vector<std::string>& areaNames,
                 std::uint32_t node, const cmed::harness::MediumOptions_t& chosen)
{
    cmed::harness::DaemonSite_t site;
    site.areaName = areaNames.at(node);
    site.uri = chosen.uri;
    site.coherency = chosen.coherency;

    // This probe restarts daemons and waits for a name to travel, so both loops turn faster than a
    // deployment's would.
    site.extra = "serve:\n  idle_interval_ms: 50\nregistry:\n  refresh_interval_ms: 100\n";
    cmed::harness::writeDaemonConfig(scratch, site);
}

[[nodiscard]] cmed::CmedClientConfig_t configFor(const cmed::harness::ProbeScratch& scratch,
                                                 const std::vector<std::string>& areaNames, std::uint32_t node)
{
    return cmed::harness::clientConfigFor(scratch, areaNames.at(node), ReachWait, LockWait);
}

// A connection is the first thing a restarted daemon can answer, so this is what confirms it's serving
// again.
[[nodiscard]] bool awaitServing(const cmed::harness::ProbeScratch& scratch,
                                const std::vector<std::string>& areaNames, std::uint32_t node)
{
    return poll::waitUntil(
        [&scratch, &areaNames, node]
        {
            try
            {
                static_cast<void>(cmed::CmedSession::connect(configFor(scratch, areaNames, node)));
                return true;
            }
            catch (const cmed::CmedError&)
            {
                return false;
            }
        },
        ReachWait, cmed::harness::DaemonStartPoll);
}

// What the requester thread and the case share. `inside` is what a remote acquire is judged against.
struct Holder_t
{
    std::atomic<bool> inside{false};
    std::atomic<bool> failed{false};
};

// Takes the domain, says so, and stays in it for @across whatever happens to its daemon.
void holdAcross(const cmed::harness::ProbeScratch& scratch, const std::vector<std::string>& areaNames,
                Holder_t& shared, timing::Millis across)
{
    try
    {
        auto session = cmed::CmedSession::connect(configFor(scratch, areaNames, 0));
        const cmed::CmedGuard guard = session.lock(HeldDomainName);
        shared.inside.store(true, std::memory_order_release);

        // What a section longer than one grant has to do. Leaving on the guard alone would keep this
        // requester inside for @across whatever happened to the daemon behind it.
        static_cast<void>(
            poll::waitUntil([&guard]
                            {
                                return !guard.stillHolds();
                            },
                            across, cmed::harness::DaemonStartPoll));
        shared.inside.store(false, std::memory_order_release);
    }
    catch (const cmed::CmedError& failure)
    {
        std::printf("holder: %s\n", failure.what());
        shared.inside.store(false, std::memory_order_release);
        shared.failed.store(true, std::memory_order_release);
    }
}

// What a forked requester does once it holds the domain: mark that fact for the parent and then
// stay, leaving only through the SIGKILL still to come rather than through its own guard.
void holdDeadSection(const cmed::harness::ProbeScratch& scratch, const std::vector<std::string>& areaNames,
                     const std::string& markerPath)
{
    auto session = cmed::CmedSession::connect(configFor(scratch, areaNames, 0));
    const cmed::CmedGuard guard = session.lock(HeldDomainName);
    static_cast<void>(guard);

    std::ofstream marker{markerPath};
    marker.close();

    std::this_thread::sleep_for(DeadSectionHold);
}

// Whether the forked requester's lock has landed, judged by the marker it leaves only once that
// lock has returned. On disk rather than in the region, so this poll needs no shared memory read.
[[nodiscard]] bool awaitDeadSectionEntered(const std::string& markerPath)
{
    return poll::waitUntil([&markerPath]
                           {
                               return ::access(markerPath.c_str(), F_OK) == 0;
                           },
                           RemoteWait, cmed::harness::DaemonStartPoll);
}

// A requester SIGKILLed inside its section leaves the state word at LockHeld and rings for nobody,
// so what has to bring the domain back is the daemon's own maintenance sweep rather than any release.
void aDeadRequestersSectionIsReclaimed(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                                       const std::vector<std::string>& areaNames, const std::string& markerPath)
{
    ctx.openCase("a dead requester's section is reclaimed");

    static_cast<void>(::unlink(markerPath.c_str()));

    const ::pid_t requester = cmed::harness::spawnChild(
        [&scratch, &areaNames, &markerPath]() -> int
        {
            holdDeadSection(scratch, areaNames, markerPath);
            return 0;
        });
    if (!ctx.check(requester > 0, "the requester starts"))
    {
        return;
    }

    const bool entered = awaitDeadSectionEntered(markerPath);
    static_cast<void>(::kill(requester, SIGKILL));  // NOLINT(misc-include-cleaner) POSIX, via <csignal>
    int status = 0;
    static_cast<void>(::waitpid(requester, &status, 0));
    static_cast<void>(::unlink(markerPath.c_str()));

    if (!ctx.check(entered, "and takes the domain before this case kills it"))
    {
        return;
    }

    // Past the hold the grant was dated for, and several maintenance ticks besides, so the sweep has
    // had its chance before anything else touches the slot.
    std::this_thread::sleep_for(SweepWait);

    bool locked = false;
    bool inherited = true;
    try
    {
        auto session = cmed::CmedSession::connect(configFor(scratch, areaNames, 0));
        const cmed::CmedGuard guard = session.lock(HeldDomainName);
        locked = static_cast<bool>(guard);

        // The kernel names a dead owner to one acquire and no other. False here says the sweep was
        // that acquire; true says this requester was, and nothing had cleared the slot before it.
        inherited = guard.wasAbandoned();
    }
    catch (const cmed::CmedError& refused)
    {
        std::printf("after the reclaim: %s\n", refused.what());
    }
    ctx.check(locked, "and a fresh requester takes the domain again");
    ctx.check(!inherited, "which finds the slot already recovered rather than inheriting the death");
}

// A killed daemon answers no release, so what bounds the requester here is its own deadline. A caller
// that waited on the daemon instead would be stuck for as long as the daemon stays dead.
void aHolderLeavesAfterTheKill(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                               const std::vector<std::string>& areaNames, cmed::harness::NodeDaemon& serving)
{
    ctx.openCase("a requester whose daemon was killed");

    auto session = cmed::CmedSession::connect(configFor(scratch, areaNames, 0));

    std::uint64_t leftAt = 0;
    {
        const cmed::CmedGuard guard = session.lock(HeldDomainName);
        if (!ctx.check(static_cast<bool>(guard), "the requester takes the domain"))
        {
            return;
        }

        serving.kill();
        leftAt = timing::monotonic<timing::Millis>();
    }

    const std::uint64_t releasing = timing::monotonic<timing::Millis>() - leftAt;
    ctx.checkf(releasing <= 2 * static_cast<std::uint64_t>(ReleaseWait.count()),
               "and leaves its section on its own deadline (%" PRIu64 "ms)", releasing);
}

// The claim a requester acts on is the area's LockHeld, backed by the daemon's cme ownership. So a
// requester still inside when another node acquires had a claim that outlived what backed it.
void aRemoteAcquireDoesNotOverlap(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                                  const std::vector<std::string>& areaNames, cmed::harness::NodeDaemon& serving,
                                  const cmed::harness::MediumOptions_t& chosen)
{
    ctx.openCase("a remote acquire while a local requester is still inside");

    Holder_t shared;
    std::thread holding{[&scratch, &areaNames, &shared]
                        {
                            holdAcross(scratch, areaNames, shared, HoldAcrossKill);
                        }};

    static_cast<void>(poll::waitUntil(
        [&shared]
        {
            return shared.inside.load(std::memory_order_acquire) ||
                   shared.failed.load(std::memory_order_acquire);
        },
        RemoteWait, cmed::harness::DaemonStartPoll));

    if (!ctx.check(shared.inside.load(std::memory_order_acquire), "node 0's requester is inside"))
    {
        holding.join();
        return;
    }

    serving.kill();
    const std::uint64_t killedAt = timing::monotonic<timing::Millis>();

    bool acquired = false;
    bool overlapped = false;
    std::uint64_t after = 0;
    try
    {
        auto second = cmed::CmedSession::connect(configFor(scratch, areaNames, 1));
        const cmed::CmedGuard guard = second.lock(HeldDomainName);
        acquired = static_cast<bool>(guard);
        overlapped = shared.inside.load(std::memory_order_acquire);
        after = timing::monotonic<timing::Millis>() - killedAt;
    }
    catch (const cmed::CmedError& refused)
    {
        std::printf("node 1: %s\n", refused.what());
    }

    holding.join();

    if (acquired)
    {
        std::printf("  node 1 acquired %" PRIu64 "ms after the kill\n", after);
    }
    ctx.check(acquired, "node 1 reaches the domain the dead daemon held");

    // KNOWN-FAIL: a turn stamp expires on its own clock, and the region hands the domain over as soon
    // as the dead daemon's peer slot is reclaimed. Nothing orders the two, so node 1 can be first.
    ctx.check(!overlapped, "and gets in only after node 0's requester has left");
}

// A name reaches another node through that node's own registry refresh, so a join on it has to be
// retried until the name is there to join.
[[nodiscard]] bool awaitJoined(cmed::CmedSession& session, const char* name)
{
    return poll::waitUntil(
        [&session, name]
        {
            try
            {
                session.joinDomain(name);
                return true;
            }
            catch (const cmed::CmedControlRefusedError&)
            {
                return false;
            }
        },
        ReachWait, cmed::harness::DaemonStartPoll);
}

// Participation is what a delete waits on, and it went with the killed peer. The requester that
// outlived the kill is what puts it back, since a replacement serves its own area and joins nothing.
void aRestartedDaemonRejoins(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                             const std::vector<std::string>& areaNames, cmed::harness::NodeDaemon& serving,
                             const cmed::harness::MediumOptions_t& chosen)
{
    ctx.openCase("what a restarted daemon puts back");

    auto second = cmed::CmedSession::connect(configFor(scratch, areaNames, 1));

    // Held across the kill and the restart, because it is the only thing that can tell the
    // replacement which domains this node is in.
    auto session = cmed::CmedSession::connect(configFor(scratch, areaNames, 0));
    session.createDomain(JoinedDomainName);
    session.joinDomain(JoinedDomainName);

    // The other node holds the domain open across the kill. Without it the domain is an orphan and the
    // case below rather than this one is what applies.
    if (!ctx.check(awaitJoined(second, JoinedDomainName), "the other node joins it too"))
    {
        return;
    }

    if (!ctx.check(countParticipants(chosen, JoinedDomainName) > 1,
                   "both nodes are participants of it"))
    {
        return;
    }

    serving.kill();

    // The killed peer's participation goes when a live peer declares it dead, and the live peer here
    // is the other node's daemon.
    std::this_thread::sleep_for(ReclaimWait);
    const std::uint32_t stranded = countParticipants(chosen, JoinedDomainName);
    ctx.checkf(stranded == 1, "the killed node's participation is reclaimed (left=%" PRIu32 ")",
               stranded);

    if (!ctx.check(serving.start() && awaitServing(scratch, areaNames, 0), "the daemon starts again"))
    {
        return;
    }

    // A migration runs on the next ask rather than on a clock, so the count moves when this lock does.
    // The join rides out with it, and the daemon raises the participation while it answers.
    bool asked = false;
    try
    {
        const cmed::CmedGuard guard = session.lock(JoinedDomainName);
        asked = static_cast<bool>(guard);
    }
    catch (const cmed::CmedError& refused)
    {
        std::printf("the surviving requester: %s\n", refused.what());
    }
    if (!ctx.check(asked, "the requester that outlived the kill reaches the domain again"))
    {
        return;
    }

    ctx.check(countParticipants(chosen, JoinedDomainName) > 1,
              "and rejoins the domain its requester still holds");

    // Whatever the area was left saying, a fresh requester has to be able to take the domain again.
    bool locked = false;
    try
    {
        auto arriving = cmed::CmedSession::connect(configFor(scratch, areaNames, 0));
        const cmed::CmedGuard guard = arriving.lock(JoinedDomainName);
        locked = static_cast<bool>(guard);
    }
    catch (const cmed::CmedError& refused)
    {
        std::printf("after the restart: %s\n", refused.what());
    }
    ctx.check(locked, "and serves a requester that arrives after it");
}

// A replacement daemon serves its own area, so a session cannot carry the killed one's mapping across.
// It greets the replacement and asks on the new area, inside the deadline the caller already gave.
void aRequesterRidesThroughAReplacement(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                                        const std::vector<std::string>& areaNames, cmed::harness::NodeDaemon& serving)
{
    ctx.openCase("a daemon replaced under a session that outlives it");

    auto session = cmed::CmedSession::connect(configFor(scratch, areaNames, 0));
    serving.kill();

    // Past the staleness bound before the ask, so the turn the killed daemon left behind cannot be
    // mistaken for one this call was granted.
    std::this_thread::sleep_for(StaleWait);

    // Asked while nothing is listening, so the call has to wait out the replacement rather than answer
    // out of the area it already holds.
    std::atomic<bool> locked{false};
    std::string threw;
    std::thread asking{[&session, &locked, &threw]()
                       {
                           try
                           {
                               const cmed::CmedGuard guard = session.lock(HeldDomainName);
                               locked.store(static_cast<bool>(guard));
                           }
                           catch (const cmed::CmedError& failure)
                           {
                               threw = failure.what();
                           }
                       }};

    const bool started = serving.start() && awaitServing(scratch, areaNames, 0);
    asking.join();

    if (!threw.empty())
    {
        std::printf("  the ask threw: %s\n", threw.c_str());
    }
    if (!ctx.check(started, "the daemon starts again"))
    {
        return;
    }
    ctx.check(locked.load(), "and the session reaches the domain through it without being told");

    // The other direction of the same contract. A replacement that never comes costs a bounded ask its
    // own deadline and no more, which is what keeps one from waiting on a node nobody is serving.
    serving.kill();
    std::this_thread::sleep_for(StaleWait);

    const timing::Deadline asked{LockWait};
    const auto empty = session.tryLock(HeldDomainName, timing::Nanos{cmed::harness::DaemonStartPoll});
    ctx.check(!empty && !asked.expired(), "and a bounded ask ends at its own deadline when none arrives");
}

// A domain lives as long as some node is in it. When the sole participant's daemon dies, recovery frees
// the slot, and no restart brings it back: the name is gone before the daemon returns.
void aSoleParticipantsDomainDoesNotSurvive(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                                           const std::vector<std::string>& areaNames, cmed::harness::NodeDaemon& serving,
                                           const cmed::harness::MediumOptions_t& chosen)
{
    ctx.openCase("a domain no other node joined");

    {
        auto session = cmed::CmedSession::connect(configFor(scratch, areaNames, 0));
        session.createDomain(SoleDomainName);
    }

    if (!ctx.check(countParticipants(chosen, SoleDomainName) == 1, "one node is in it and no other"))
    {
        return;
    }

    // The other node acquires while this one is dead, because the reclaim is work a live peer does and an
    // idle peer has no reason to do it. Waiting alone would leave the orphan standing.
    auto second = cmed::CmedSession::connect(configFor(scratch, areaNames, 1));
    static_cast<void>(awaitJoined(second, JoinedDomainName));

    serving.kill();

    // Waited on rather than slept through: how long the reclaim takes is the region's business rather
    // than a number this case can pick.
    const timing::Deadline reclaiming{ReclaimWait};
    while (namePresent(chosen, SoleDomainName) && !reclaiming.expired())
    {
        try
        {
            const cmed::CmedGuard turning = second.lock(JoinedDomainName);
        }
        catch (const cmed::CmedError&)
        {
            // @expected: the other node's own troubles are not this case's subject.
        }
        std::this_thread::sleep_for(cmed::harness::DaemonStartPoll);
    }
    if (!ctx.check(!namePresent(chosen, SoleDomainName), "the region frees the domain it orphaned"))
    {
        return;
    }

    if (!ctx.check(serving.start() && awaitServing(scratch, areaNames, 0), "the daemon starts again"))
    {
        return;
    }

    // The answer a requester has to be given. Anything else would have it wait on a name the region no
    // longer carries.
    bool absent = false;
    try
    {
        auto session = cmed::CmedSession::connect(configFor(scratch, areaNames, 0));
        static_cast<void>(session.lock(SoleDomainName));
    }
    catch (const cmed::CmedUnknownDomainError&)
    {
        absent = true;
    }
    ctx.check(absent, "and a requester is told the domain is gone rather than left waiting");
}

// A shm object keeps the size it was created with, so more peer slots than the last run would format
// into a mapping too small for them. Only this scheme; a dax device or file belongs to whoever set it up.
void removeRegion(const cmed::harness::MediumOptions_t& chosen)
{
    constexpr std::string_view ShmScheme{"shm:"};
    if (chosen.uri.compare(0, ShmScheme.size(), ShmScheme) == 0)
    {
        static_cast<void>(::shm_unlink(chosen.uri.c_str() + ShmScheme.size()));
    }
}

}  // namespace

int main(int argc, char** argv)
{
    cmed::harness::ProbeScratch scratch{"crash-probe"};

    cmed::harness::MediumOptions_t chosen;
    chosen.parse(argc, argv);

    // Held for the whole run, because its destructor is what takes the area away afterwards. Empty
    // when --uri named a medium the caller owns.
    std::unique_ptr<harness::TestMemory> area;
    try
    {
        area = chosen.resolve();
    }
    catch (const harness::MediumUnavailable& absent)
    {
        std::printf("SKIP: %s\n", absent.what());
        return harness::SkipExitCode;
    }

    std::vector<std::string> areaNames;
    areaNames.reserve(Nodes);
    for (std::uint32_t node = 0; node < Nodes; ++node)
    {
        areaNames.push_back(scratch.makeAreaName(std::to_string(node)));
    }

    // On disk rather than in the region, so a poll from this process needs no shared memory read to
    // see that a forked child's lock succeeded.
    const std::string markerPath = scratch.makePath("dead-section.marker");

    bool passed = false;
    try
    {
        removeRegion(chosen);
        // Held in an optional so it can be let go while the daemons are still up: leaving a domain whose
        // holder is a killed peer waits on the region's own reclaim, which needs a live daemon to finish.
        std::optional<cmed::harness::ProbeRegion> region;
        region.emplace(chosen.uri, RegionSlots, RegionPeers, chosen.coherencyMode());
        region->createDomain(HeldDomainName);

        writeConfig(scratch, areaNames, 0, chosen);
        writeConfig(scratch, areaNames, 1, chosen);

        cmed::harness::NodeDaemon first{makeConfigPath(scratch, areaNames, 0)};
        const cmed::harness::NodeDaemon second{makeConfigPath(scratch, areaNames, 1)};
        if (!first.started() || !second.started())
        {
            std::printf("crash probe could not start a daemon\n");
            return 1;
        }

        if (!awaitServing(scratch, areaNames, 0) || !awaitServing(scratch, areaNames, 1))
        {
            std::printf("crash probe: a daemon never answered\n");
            return 1;
        }

        passed = probe::run(
                     "crash probe",
                     [&first, &chosen, &scratch, &areaNames, &markerPath](probe::Context& ctx)
                     {
                         aDeadRequestersSectionIsReclaimed(ctx, scratch, areaNames, markerPath);

                         aHolderLeavesAfterTheKill(ctx, scratch, areaNames, first);

                         if (!first.start() || !awaitServing(scratch, areaNames, 0))
                         {
                             ctx.check(false, "node 0's daemon starts again for the next case");
                             return;
                         }
                         aRemoteAcquireDoesNotOverlap(ctx, scratch, areaNames, first, chosen);

                         if (!first.start() || !awaitServing(scratch, areaNames, 0))
                         {
                             ctx.check(false, "node 0's daemon starts again for the last case");
                             return;
                         }
                         aRestartedDaemonRejoins(ctx, scratch, areaNames, first, chosen);

                         aSoleParticipantsDomainDoesNotSurvive(ctx, scratch, areaNames, first, chosen);

                         if (!first.start() || !awaitServing(scratch, areaNames, 0))
                         {
                             ctx.check(false, "node 0's daemon starts again for the last case");
                             return;
                         }
                         aRequesterRidesThroughAReplacement(ctx, scratch, areaNames, first);
                     }) == 0;

        // While the daemons are still up, so this session's leave has a live peer to hand to.
        region.reset();
    }
    catch (const std::exception& failure)
    {
        std::printf("crash probe threw: %s\n", failure.what());
        passed = false;
    }

    return passed ? 0 : 1;
}
