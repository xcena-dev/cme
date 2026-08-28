// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// crossproc_probe.cpp -- the requester's exclusion and its recovery, between processes.
//
// lock_probe.cpp runs holders as threads of one process, leaving two attributes untested from the
// requester's side: threads exclude each other through a plain mutex, and a dying thread takes the
// process with it. Here each holder is a separate process with its own CmedSession, and two of them
// die inside the span, forked while this process is still single threaded.

#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "cmed/guard.hpp"
#include "cmed/session.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "harness/helper.hpp"
#include "harness/helper_requester.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/area.hpp"
#include "shared/posix/unique_fd.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

// Two domains: the two halves of the death case consume the same evidence, since reading EOWNERDEAD
// off a mutex makes it consistent, so each needs a holder of its own to have died.
constexpr const char* ContendedName = "lane0";
constexpr const char* AbandonedName = "lane1";

constexpr std::uint32_t ContendedSlot = cmed::harness::FirstDataSlot;
constexpr std::uint32_t AbandonedSlot = ContendedSlot + 1;

// Never published, so a lookup never reaches them and their words are free for the probe's own
// bookkeeping, the only memory this process and a forked child share.
constexpr std::uint32_t ScratchSlot = ContendedSlot + 2;
constexpr std::uint32_t PhaseSlot = ContendedSlot + 3;

constexpr std::uint32_t Contenders = 3;
constexpr std::uint32_t Rounds = 6;

constexpr std::uint32_t SetupPhase = 0;
constexpr std::uint32_t ContendPhase = 1;
constexpr std::uint32_t DoomPhase = 2;

// Long enough that a second holder entering the span would be seen. An acquire and release with
// nothing between them passes whether or not anything excluded the other process.
constexpr timing::Millis HoldFor{2};

std::atomic<std::uint32_t>& phaseWord(cmed::protocol::SharedArea_t& area) noexcept
{
    return cmed::harness::resolveWaitersWord(area, PhaseSlot);
}

std::atomic<std::uint32_t>& insideWord(cmed::protocol::SharedArea_t& area) noexcept
{
    return cmed::harness::resolveWaitersWord(area, ScratchSlot);
}

std::atomic<std::int32_t>& overlapWord(cmed::protocol::SharedArea_t& area) noexcept
{
    return cmed::harness::resolveResultWord(area, ScratchSlot);
}

std::atomic<std::uint32_t>& finishedWord(cmed::protocol::SharedArea_t& area) noexcept
{
    return cmed::harness::resolveSeqWord(area, ScratchSlot);
}

// Bounded, so a child of a run that went wrong exits instead of holding the suite until its
// timeout.
bool awaitPhase(cmed::protocol::SharedArea_t& area, std::uint32_t wanted)
{
    return poll::waitUntil([&area, wanted]
                           {
                               return phaseWord(area).load(std::memory_order_acquire) == wanted;
                           },
                           timing::Secs{10}, timing::Millis{1});
}

// A child reports through the area and never through probe::Context: its checks would print, and its
// tally would die with it. The parent reads these words and records the checks itself.
int contend(cmed::CmedSession& session, cmed::protocol::SharedArea_t& area)
{
    for (std::uint32_t round = 0; round < Rounds; ++round)
    {
        const cmed::CmedGuard guard = session.lock(ContendedName);
        if (!guard)
        {
            return 3;
        }

        if (insideWord(area).fetch_add(1, std::memory_order_acq_rel) != 0)
        {
            overlapWord(area).store(1, std::memory_order_release);
        }
        std::this_thread::sleep_for(HoldFor);
        if (insideWord(area).load(std::memory_order_acquire) != 1)
        {
            overlapWord(area).store(1, std::memory_order_release);
        }
        insideWord(area).fetch_sub(1, std::memory_order_acq_rel);
        finishedWord(area).fetch_add(1, std::memory_order_release);
    }
    return 0;
}

// Takes the domain and never gives it back. The exit is what leaves the mutex abandoned and the
// state stuck at LockHeld, which is the state the next requester has to recover from.
[[noreturn]] void dieHolding(cmed::CmedSession& session, const char* domainName)
{
    const cmed::CmedGuard guard = session.lock(domainName);
    // _exit, not return: an unwound stack would run the guard's destructor and give the turn back.
    ::_exit(guard ? 0 : 3);
}

enum class Role
{
    Contender,
    DiesOnContended,
    DiesOnAbandoned,
};

// The descriptor comes from the parent, inherited across the fork, and the mapping it makes is this
// child's own: what the case is about is one process seeing what another wrote. The session comes
// through the parent's door instead, since a descriptor greets nobody.
int runChild(Role role, posix::FileDesc areaDescriptor, const std::string& socketPath)
{
    auto session = cmed::CmedSession::connect(socketPath);
    cmed::CmedArea area = cmed::harness::attachAgain(areaDescriptor);

    if (role != Role::Contender)
    {
        if (!awaitPhase(area.shared(), DoomPhase))
        {
            return 2;
        }
        dieHolding(session, role == Role::DiesOnContended ? ContendedName : AbandonedName);
    }

    if (!awaitPhase(area.shared(), ContendPhase))
    {
        return 2;
    }
    return contend(session, area.shared());
}

::pid_t spawn(Role role, posix::FileDesc areaDescriptor, const std::string& socketPath)
{
    return cmed::harness::spawnChild([role, areaDescriptor, &socketPath]
                                     {
                                         return runChild(role, areaDescriptor, socketPath);
                                     });
}

// Two cycles, not one. A lock that took the abandoned mutex without pthread_mutex_consistent
// leaves it NOTRECOVERABLE on release, so the first cycle would still pass and the second not.
void theNextRequesterStillGetsTheTurn(probe::Context& ctx, cmed::harness::ProbeArea& area,
                                      const cmed::harness::StubSetup& setup)
{
    cmed::CmedSession session = setup.openRequester();
    for (std::uint32_t attempt = 1; attempt <= 2; ++attempt)
    {
        cmed::CmedGuard guard = session.lock(AbandonedName);
        ctx.checkf(static_cast<bool>(guard) && cmed::harness::isHeld(area.shared(), AbandonedSlot),
                   "cycle %" PRIu32 " takes the domain a dead holder left", attempt);

        // Only the first cycle inherits the dead child's span; the second takes what the first
        // released cleanly. A guard that reported the same on both would be reporting nothing.
        ctx.checkf(guard.wasAbandoned() == (attempt == 1),
                   "cycle %" PRIu32 " reports wasAbandoned() == %s", attempt, attempt == 1 ? "true" : "false");

        // Where wasAbandoned() answers for one acquire, this answers for the domain, so the second
        // cycle reads the same count the first raised.
        ctx.checkf(guard.readAbandonCount() == 1, "cycle %" PRIu32 " reads one abandonment outstanding",
                   attempt);

        // The second cycle, so the check above has already shown the count outliving the acquire that
        // raised it. Inside this guard rather than a third one, which the grant count below would see.
        if (attempt == 2)
        {
            ctx.check(!guard.clearAbandonCount(0), "a clear against a count nobody read is refused");
            ctx.check(guard.clearAbandonCount(1), "and a clear against the count that stands is taken");
            ctx.check(guard.readAbandonCount() == 0, "which leaves nothing outstanding");
        }
    }

    ctx.check(cmed::harness::isIdle(area.shared(), AbandonedSlot), "and it ends at Idle");
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"crossproc-probe"};
    return probe::run(
        "crossproc probe",
        [&scratch](probe::Context& ctx)
        {
            const std::string areaName = scratch.makeAreaName("");
            cmed::harness::ProbeArea area{areaName.c_str()};
            cmed::harness::publishDomains(area.shared(), cmed::harness::FirstDataSlot,
                                          {ContendedName, AbandonedName});
            phaseWord(area.shared()).store(SetupPhase, std::memory_order_release);
            const cmed::harness::StubSetup setup{scratch.makePath("cmed.sock"), area.descriptor()};

            std::vector<::pid_t> contenders;
            contenders.reserve(Contenders);
            for (std::uint32_t index = 0; index < Contenders; ++index)
            {
                contenders.push_back(spawn(Role::Contender, area.descriptor(), setup.path()));
            }
            const ::pid_t diesOnContended = spawn(Role::DiesOnContended, area.descriptor(), setup.path());
            const ::pid_t diesOnAbandoned = spawn(Role::DiesOnAbandoned, area.descriptor(), setup.path());

            const cmed::harness::StubDaemon daemon{area.shared()};
            phaseWord(area.shared()).store(ContendPhase, std::memory_order_release);

            bool everyContenderExited = true;
            for (const ::pid_t contender : contenders)
            {
                everyContenderExited = cmed::harness::reapedCleanly(contender) && everyContenderExited;
            }

            ctx.openCase("several processes over one domain");
            ctx.check(everyContenderExited, "every contender finished its rounds");
            ctx.check(overlapWord(area.shared()).load(std::memory_order_acquire) == 0,
                      "no two processes were inside the span at once");
            ctx.checkf(finishedWord(area.shared()).load(std::memory_order_acquire) == Contenders * Rounds,
                       "all %" PRIu32 " sections ran", Contenders * Rounds);

            phaseWord(area.shared()).store(DoomPhase, std::memory_order_release);

            ctx.openCase("a holder that dies inside the span");
            ctx.check(cmed::harness::reapedCleanly(diesOnContended) &&
                          cmed::harness::reapedCleanly(diesOnAbandoned),
                      "both doomed children reached the span and died in it");
            ctx.check(cmed::harness::isHeld(area.shared(), ContendedSlot),
                      "the domain is left at LockHeld");
            // Read straight off the mutex. Without this the recovery case below could pass against
            // a child that never reached the span at all.
            ctx.check(cmed::harness::wasLeftAbandoned(
                          cmed::harness::resolveDomainMutex(area.shared(), ContendedSlot)),
                      "and the mutex says its holder never released it");

            ctx.openCase("the domain a dead holder left");
            theNextRequesterStillGetsTheTurn(ctx, area, setup);

            ctx.openCase("what the daemon was asked for");

            // Every contender's round plus the child that died holding it.
            constexpr std::uint32_t ContendedAcquires = (Contenders * Rounds) + 1;

            // The child that died holding it, then the two recovery cycles.
            constexpr std::uint32_t AbandonedAcquires = 3;

            constexpr std::uint32_t Expected = ContendedAcquires + AbandonedAcquires;
            static_cast<void>(daemon.awaitGrants(Expected));
            ctx.checkf(daemon.grants() == Expected, "%" PRIu32 " grants, one per acquire", daemon.grants());

            // Which slot each acquire reached, since the total alone reads the same whether or not a
            // name resolved to the domain it belongs to.
            ctx.checkf(daemon.grantsFor(ContendedSlot) == ContendedAcquires,
                       "%" PRIu32 " of them against the contended domain", daemon.grantsFor(ContendedSlot));
            ctx.checkf(daemon.grantsFor(AbandonedSlot) == AbandonedAcquires,
                       "%" PRIu32 " against the domain a holder died on", daemon.grantsFor(AbandonedSlot));
        });
}
