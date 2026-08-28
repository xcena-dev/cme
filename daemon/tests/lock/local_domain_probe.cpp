// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// local_domain_probe.cpp -- what this node holds for one domain, and the lock that is the only way in.
//
// No region and no daemon. The turn is driven with an empty cme::Guard, because what the entry
// answers is whether it holds one at all, and a Guard that held a domain would need a region under it.
//
// The unlocked read ends the run by design, so that case runs in a child and reads the signal it
// died of.

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <cinttypes>
#include <csignal>
#include <cstdint>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "daemon/domain/local_domain.hpp"
#include "harness/helper_process.hpp"
#include "shared/protocol/shared_area.hpp"
#include "tests/probe_context.hpp"

namespace
{

// The slot this node's entry stands for. Value-initialised, so every word reads as a fresh area's.
cmed::protocol::Domain_t g_half{};

constexpr std::uint32_t PairedId = 7;

// An id and the incarnation the join answered with, which is what a leave has to hand back.
constexpr cme::DomainHandle_t Resolved{PairedId, 41};

// Long enough that no case reaches it while running.
constexpr timing::Secs Hold{5};

void pairSlotIsOutsideTheLock(probe::Context& ctx)
{
    ctx.openCase("the two halves an entry stands for");

    cmed::daemon::LocalDomain entry;
    entry.pairSlot(PairedId, g_half);

    // Paired once at startup and read without the lock, because neither half of a domain moves and
    // a requester takes the shm half's own robust lock without seeing this mutex.
    ctx.checkf(entry.readId() == PairedId, "the id answers with no Guard held (%" PRIu32 ")", entry.readId());
    ctx.check(&entry.slot() == &g_half, "and the slot is the one it was paired with");
}

void theJoinCountStopsAtZero(probe::Context& ctx)
{
    ctx.openCase("joins counted, and dropped");

    cmed::daemon::LocalDomain entry;
    const cmed::daemon::LocalDomain::Guard holding{entry};

    holding->addJoin();
    holding->addJoin();
    ctx.check(holding->dropJoin(), "a join that was made can be dropped");
    ctx.check(holding->dropJoin(), "and so can the second");

    // A count that wrapped would never reach zero again, so the node would stay inside a domain no
    // other node may then delete.
    ctx.check(!holding->dropJoin(), "a third drop answers false rather than wrapping past zero");
    ctx.check(!holding->keepsParticipation(), "and nothing is left needing this node in the domain");
}

void clearJoinsDropsThemAllAtOnce(probe::Context& ctx)
{
    ctx.openCase("every join at once");

    cmed::daemon::LocalDomain entry;
    const cmed::daemon::LocalDomain::Guard holding{entry};

    holding->addJoin();
    holding->addJoin();
    holding->addJoin();
    holding->clearJoins();

    ctx.check(!holding->keepsParticipation(), "a cleared entry needs nothing");
    ctx.check(!holding->dropJoin(), "and there is nothing left to drop");
}

void participationAnswersForTheTurnAsWellAsTheJoins(probe::Context& ctx)
{
    ctx.openCase("what keeps this node inside a domain");

    cmed::daemon::LocalDomain entry;
    const cmed::daemon::LocalDomain::Guard holding{entry};

    ctx.check(!holding->keepsParticipation(), "a fresh entry keeps nothing");

    holding->addJoin();
    ctx.check(holding->keepsParticipation(), "one requester's join keeps it");
    ctx.check(holding->dropJoin() && !holding->keepsParticipation(), "and dropping that join lets it go");

    holding->takeTurn(cme::Guard{}, Resolved, Hold);
    ctx.check(holding->hasTurn(), "a turn taken is held");
    ctx.check(holding->keepsParticipation(), "and keeps the node in with no join behind it");

    holding->releaseTurn();
    ctx.check(!holding->hasTurn(), "handing the turn back leaves none");
    ctx.check(!holding->keepsParticipation(), "and nothing keeping the node in");
}

void theTurnCarriesTheHandleItWasTakenOn(probe::Context& ctx)
{
    ctx.openCase("the handle a turn was taken on");

    cmed::daemon::LocalDomain entry;
    const cmed::daemon::LocalDomain::Guard holding{entry};
    holding->takeTurn(cme::Guard{}, Resolved, Hold);

    // The incarnation is what refuses a slot that changed hands, so a leave by this handle leaves the
    // domain the turn was taken for.
    ctx.check(holding->turnHandle().id == Resolved.id, "the id comes back as it went in");
    ctx.checkf(holding->turnHandle().incarnation == Resolved.incarnation,
               "and so does the incarnation the join answered (%" PRIu64 ")", holding->turnHandle().incarnation);
}

void aHoldOfNothingIsExpiredWhenItIsTaken(probe::Context& ctx)
{
    ctx.openCase("how long a turn may be kept");

    cmed::daemon::LocalDomain entry;
    const cmed::daemon::LocalDomain::Guard holding{entry};

    holding->takeTurn(cme::Guard{}, Resolved, timing::Millis::zero());
    ctx.check(holding->runExpired(), "a turn with no hold on it is expired the moment it is taken");

    holding->takeTurn(cme::Guard{}, Resolved, Hold);
    ctx.check(!holding->runExpired(), "and one with seconds on it is not");
}

// Whether a read with no Guard held ends the child, and by which signal. The core limit is dropped
// first, so a passing case does not leave a core file behind.
[[nodiscard]] bool diesReadingWithoutTheGuard(cmed::daemon::LocalDomain& entry)
{
    const ::pid_t child = cmed::harness::spawnChild(
        [&entry]
        {
            const ::rlimit noCore = {};
            static_cast<void>(::setrlimit(RLIMIT_CORE, &noCore));
            static_cast<void>(entry.hasTurn());
            return 0;
        });
    if (child == 0)
    {
        return false;
    }

    int status = 0;
    return ::waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

void theGuardIsTheOnlyWayIn(probe::Context& ctx)
{
    ctx.openCase("a read with nobody holding the lock");

    cmed::daemon::LocalDomain entry;
    ctx.check(diesReadingWithoutTheGuard(entry), "ends the run rather than answering from a racing count");

    const cmed::daemon::LocalDomain::Guard holding{entry};
    ctx.check(!holding->hasTurn(), "while the same read under a Guard answers");
}

void theLockIsGivenBackWhenTheGuardGoes(probe::Context& ctx)
{
    ctx.openCase("one Guard after another");

    cmed::daemon::LocalDomain entry;
    {
        const cmed::daemon::LocalDomain::Guard holding{entry};
        holding->addJoin();
    }

    // A second Guard on the same entry would block forever if the first had not given the mutex
    // back, so reaching the check at all is half of what it asserts.
    const cmed::daemon::LocalDomain::Guard again{entry};
    ctx.check(again->keepsParticipation(), "the next holder sees what the last one left");
}

}  // namespace

int main()
{
    return probe::run("local domain probe",
                      [](probe::Context& ctx)
                      {
                          pairSlotIsOutsideTheLock(ctx);
                          theJoinCountStopsAtZero(ctx);
                          clearJoinsDropsThemAllAtOnce(ctx);
                          participationAnswersForTheTurnAsWellAsTheJoins(ctx);
                          theTurnCarriesTheHandleItWasTakenOn(ctx);
                          aHoldOfNothingIsExpiredWhenItIsTaken(ctx);
                          theGuardIsTheOnlyWayIn(ctx);
                          theLockIsGivenBackWhenTheGuardGoes(ctx);
                      });
}
