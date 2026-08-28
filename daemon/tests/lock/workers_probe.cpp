// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// workers_probe.cpp -- who a domain is handed to, and the two answers that are not a hand-over.
//
// The dispatcher reads a false from assign() as "leave the bit up and come back", so a false that
// should have been true costs one domain a delay, and a true that should have been false puts two
// workers on one domain. Neither shows in a run where no two hand-overs overlap, so every case that
// asks about the mask keeps a worker inside its serve while it asks.

#include <cinttypes>
#include <cstdint>
#include <string>
#include <vector>

#include "cme/shared.hpp"
#include "cmed/errors.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "daemon/domain/manager.hpp"
#include "daemon/observe/counters.hpp"
#include "daemon/serve/workers.hpp"
#include "daemon/startup/config.hpp"
#include "harness/helper.hpp"
#include "harness/helper_cme_region.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

// A worker with no spin parks in the kernel at once, which is the state a stop has to reach with a
// wake rather than by outliving a timeout.
constexpr timing::Micros NoSpin{0};

constexpr std::uint32_t FirstSlot = cmed::harness::FirstDataSlot;
constexpr std::uint32_t SecondSlot = FirstSlot + 1;
constexpr std::uint32_t ThirdSlot = FirstSlot + 2;

// The control domain and the three data slots the cases here name.
constexpr std::uint32_t RegionSlots = 4;

// Far past any case, so a turn goes back because a case gave it back rather than because the hold
// ran out under it.
constexpr timing::Millis CohortHold{10000};

[[nodiscard]] std::uint64_t bitFor(std::uint32_t domainId) noexcept
{
    return std::uint64_t{1} << domainId;
}

// Nothing announces a worker finishing, so the only way to see the in-flight bit come off is to look
// again.
[[nodiscard]] bool awaitIdle(const cmed::daemon::DomainWorkers& working, std::uint32_t domainId)
{
    return poll::waitUntil(
        [&working, domainId]
        {
            return !working.isBusy(domainId);
        },
        cmed::harness::ProbeAttachWait, cmed::harness::ProbePoll);
}

// A worker offers itself as idle only once its thread has run, so a hand-over made the instant the
// pool was built is refused for want of one. The dispatcher comes back for it, and so does this.
[[nodiscard]] bool awaitAssign(cmed::daemon::DomainWorkers& working, std::uint32_t domainId)
{
    return poll::waitUntil(
        [&working, domainId]
        {
            return working.assign(domainId);
        },
        cmed::harness::ProbeAttachWait, cmed::harness::ProbePoll);
}

// Thrown to leave a scope the way a failure would. The destructor a throw reaches is the subject, so
// what the exception carries does not matter.
struct LeavingTheScope_t
{
};

[[nodiscard]] cmed::daemon::DaemonConfig_t configWithHold(timing::Millis cohortHold)
{
    cmed::daemon::DaemonConfig_t config;
    config.cohort.hold = cohortHold;
    return config;
}

// Both halves of a domain, plus a second peer that can hold the region's turn for it. A worker
// granting a domain whose turn that peer holds stays inside its serve until the turn is let go, and
// that is what keeps a domain in flight while a case asks about the mask.
class ServedDomains
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // @domainCount data domains from FirstSlot upwards, created in the region and published in the
    // area at the same ids: a create takes the lowest free record, so the two sides agree.
    ServedDomains(cmed::harness::ProbeScratch& scratch, std::uint32_t domainCount)
        : domainCount_{domainCount},
          area_{"workers-probe"},
          config_{configWithHold(CohortHold)},
          regionName_{scratch.makeAreaName("region")},
          region_{regionName_.c_str(), RegionSlots},
          contender_{cme::Session::open(std::string{"shm:"} + regionName_)},
          domains_{region_.session(), area_.shared(), config_}
    {
        for (std::uint32_t domainId = FirstSlot; domainId < FirstSlot + domainCount_; ++domainId)
        {
            const std::string name = nameFor(domainId);
            region_.createDomain(name);
            cmed::harness::publishDomain(area_.shared(), domainId, name);
            static_cast<void>(contender_.joinDomain(name));
        }
    }

    ServedDomains(const ServedDomains&) = delete;
    ServedDomains(ServedDomains&&) = delete;
    ~ServedDomains() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────
    ServedDomains& operator=(const ServedDomains&) = delete;
    ServedDomains& operator=(ServedDomains&&) = delete;

    // ── public methods ─────────────────────────────────────────────
    // The region's turn for every domain, taken by the second peer. A worker handed one of them then
    // waits inside the acquire, which is where a case wants it.
    void holdEveryTurn()
    {
        for (std::uint32_t domainId = FirstSlot; domainId < FirstSlot + domainCount_; ++domainId)
        {
            turns_.push_back(contender_.lock(nameFor(domainId)));
        }
    }

    void letTurnsGo() noexcept
    {
        turns_.clear();
    }

    // What makes a serve do anything at all: a serve of a slot nobody asked about returns without
    // touching the region.
    void askForLock(std::uint32_t domainId)
    {
        cmed::harness::resolveSlot(area_.shared(), domainId).publish(cmed::protocol::RequestState::LockRequested);
    }

    // ── settling ───────────────────────────────────────────────────
    // Granting is published before the acquire, so a slot standing in it is one a worker is inside.
    // @wanted of them at once is what a case needs before it may ask whether a further hand-over is refused.
    [[nodiscard]] bool awaitGranting(std::uint32_t wanted)
    {
        return poll::waitUntil(
            [this, wanted]
            {
                return granting() >= wanted;
            },
            cmed::harness::ProbeAttachWait, cmed::harness::ProbePoll);
    }

    [[nodiscard]] bool awaitHeld(std::uint32_t domainId)
    {
        return poll::waitUntil(
            [this, domainId]
            {
                return cmed::harness::isHeld(area_.shared(), domainId);
            },
            cmed::harness::ProbeAttachWait, cmed::harness::ProbePoll);
    }

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] cmed::daemon::DomainManager& domains() noexcept
    {
        return domains_;
    }

    [[nodiscard]] std::uint64_t counted(cmed::observe::DomainEvent asked) const
    {
        return domains_.readCount(asked);
    }

    // How many slots stand in Granting, so a case reads one number rather than a state per slot.
    [[nodiscard]] std::uint32_t granting()
    {
        std::uint32_t inside = 0;
        for (std::uint32_t domainId = FirstSlot; domainId < FirstSlot + domainCount_; ++domainId)
        {
            if (cmed::harness::readState(area_.shared(), domainId) == cmed::protocol::RequestState::Granting)
            {
                ++inside;
            }
        }
        return inside;
    }

private:
    [[nodiscard]] static std::string nameFor(std::uint32_t domainId)
    {
        return "served-" + std::to_string(domainId);
    }

private:
    std::uint32_t domainCount_;

    // Its own area per fixture, so no case reads a slot another one left standing.
    cmed::harness::ProbeArea area_;

    // Before the manager, which holds a pointer to it for as long as it serves.
    cmed::daemon::DaemonConfig_t config_;

    // Before the region, whose name is a pointer into this.
    std::string regionName_;
    cmed::harness::ProbeRegion region_;

    // The peer this fixture contends with, and the turns it is holding right now.
    cme::Session contender_;
    std::vector<cme::Guard> turns_;

    // Last, so the region and the area it is paired with are both built before it reads them.
    cmed::daemon::DomainManager domains_;
};

void aCountTheIdleWordCannotHoldIsRefused(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a worker count the idle word cannot hold");

    ServedDomains serving{scratch, 1};

    const auto refuses = [&serving](std::uint32_t count)
    {
        try
        {
            const cmed::daemon::DomainWorkers working{serving.domains(), count, NoSpin};
            return false;
        }
        catch (const cmed::CmedInvalidArgumentError&)
        {
            return true;
        }
    };

    ctx.check(refuses(0), "a pool of no workers is refused");
    ctx.check(refuses(cmed::daemon::DomainWorkers::MostWorkers + 1),
              "and one worker past what the idle word holds");

    // The boundary the other way. An off-by-one that refused this count would cost the largest pool
    // the design allows, and one that accepted the count above would shift a bit out of the word.
    bool handed = false;
    bool served = false;
    {
        serving.askForLock(FirstSlot);
        cmed::daemon::DomainWorkers working{serving.domains(), cmed::daemon::DomainWorkers::MostWorkers,
                                            NoSpin};
        handed = awaitAssign(working, FirstSlot);

        // Awaited inside the scope. A worker reads the stop at the top of its loop, so one handed a
        // domain it has not picked up yet exits without serving it.
        served = serving.awaitHeld(FirstSlot);
    }
    ctx.check(handed, "a pool of exactly what the word holds is built and hands a domain out");
    ctx.check(served, "and that hand-over reaches a worker, which answers the request");
}

void aDomainReachesTheWorkerItWasHandedTo(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("the domain a worker is handed");

    ServedDomains serving{scratch, 2};
    serving.holdEveryTurn();
    serving.askForLock(FirstSlot);

    cmed::daemon::DomainWorkers working{serving.domains(), 1, NoSpin};

    ctx.check(awaitAssign(working, FirstSlot), "one idle worker takes the domain");
    if (!ctx.check(serving.awaitGranting(1), "and the worker is inside the serve for it"))
    {
        serving.letTurnsGo();
        return;
    }

    ctx.checkf(working.busy().takeSnapshot().getWord(0) == bitFor(FirstSlot),
               "the mask carries that domain and no other, at %" PRIx64,
               working.busy().takeSnapshot().getWord(0));
    ctx.checkf(working.readCount(cmed::observe::WorkerEvent::Handed) == 1,
               "and one hand-over was counted, at %" PRIu64,
               working.readCount(cmed::observe::WorkerEvent::Handed));

    serving.letTurnsGo();
    ctx.check(serving.awaitHeld(FirstSlot), "the serve answers the request once the turn is free");
    ctx.check(awaitIdle(working, FirstSlot), "the in-flight bit comes off when the serve returns");
    ctx.checkf(working.busy().takeSnapshot().getWord(0) == 0, "leaving the whole mask clear, at %" PRIx64,
               working.busy().takeSnapshot().getWord(0));
}

void aDomainAlreadyWithAWorkerIsRefused(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a second hand-over of a domain a worker still holds");

    ServedDomains serving{scratch, 2};
    serving.holdEveryTurn();
    serving.askForLock(FirstSlot);

    // Two workers, so what refuses the second hand-over is the domain's own bit rather than an empty
    // pool. Those are the two falses assign() answers with, and one worker cannot tell them apart.
    cmed::daemon::DomainWorkers working{serving.domains(), 2, NoSpin};

    ctx.check(awaitAssign(working, FirstSlot), "the first hand-over is taken");
    if (!ctx.check(serving.awaitGranting(1), "and the worker is inside the serve"))
    {
        serving.letTurnsGo();
        return;
    }

    ctx.check(!working.assign(FirstSlot), "the second is refused while that worker holds the domain");
    ctx.check(working.isBusy(FirstSlot), "which is the same fact isBusy answers");
    ctx.checkf(working.busy().takeSnapshot().getWord(0) == bitFor(FirstSlot),
               "and the mask carries that bit alone, at %" PRIx64,
               working.busy().takeSnapshot().getWord(0));
    ctx.checkf(working.readCount(cmed::observe::WorkerEvent::Handed) == 1,
               "with the refusal counted as no hand-over, at %" PRIu64,
               working.readCount(cmed::observe::WorkerEvent::Handed));

    serving.letTurnsGo();
    ctx.check(serving.awaitHeld(FirstSlot), "the held serve answers the request");
    ctx.check(awaitIdle(working, FirstSlot), "and the bit comes off");
    ctx.checkf(serving.counted(cmed::observe::DomainEvent::Grants) == 1,
               "the refused hand-over reached no serve, at %" PRIu64 " grants",
               serving.counted(cmed::observe::DomainEvent::Grants));
}

void aPoolWithNoIdleWorkerIsRefused(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a hand-over with every worker already at work");

    ServedDomains serving{scratch, 3};
    serving.holdEveryTurn();
    serving.askForLock(FirstSlot);
    serving.askForLock(SecondSlot);
    serving.askForLock(ThirdSlot);

    cmed::daemon::DomainWorkers working{serving.domains(), 2, NoSpin};

    const bool tookFirst = awaitAssign(working, FirstSlot);
    const bool tookSecond = awaitAssign(working, SecondSlot);
    ctx.check(tookFirst && tookSecond, "two idle workers take two domains");
    if (!ctx.check(serving.awaitGranting(2), "and both are inside their serve"))
    {
        serving.letTurnsGo();
        return;
    }

    ctx.check(!working.assign(ThirdSlot), "a third domain is refused with no worker left to take it");

    // The claim comes back off, and that is the branch worth a case: a claim left on would make the
    // domain unassignable until a worker that never took it cleared it.
    ctx.check(!working.isBusy(ThirdSlot), "the claim on it comes back off");
    ctx.checkf(working.busy().takeSnapshot().getWord(0) == (bitFor(FirstSlot) | bitFor(SecondSlot)),
               "leaving the mask on the two domains that were taken, at %" PRIx64,
               working.busy().takeSnapshot().getWord(0));

    serving.letTurnsGo();
    ctx.check(serving.awaitHeld(FirstSlot) && serving.awaitHeld(SecondSlot), "the two held serves answer");
    ctx.check(awaitIdle(working, FirstSlot) && awaitIdle(working, SecondSlot), "and both bits come off");

    ctx.check(awaitAssign(working, ThirdSlot), "the third domain is taken once a worker is idle again");
    ctx.check(serving.awaitHeld(ThirdSlot), "and its serve answers");
    ctx.checkf(serving.counted(cmed::observe::DomainEvent::Grants) == 3, "for three grants in all, at %" PRIu64,
               serving.counted(cmed::observe::DomainEvent::Grants));
}

void anIdPastTheTableIsNotAHandOver(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("an id past the domain table");

    ServedDomains serving{scratch, 0};
    cmed::daemon::DomainWorkers working{serving.domains(), 1, NoSpin};

    // True, which the dispatcher reads as "nothing left to come back for" rather than as a hand-over.
    ctx.check(working.assign(cmed::MaxDomains), "an id one past the table is not deferred");
    ctx.check(working.assign(cmed::protocol::NoDomain), "nor is the id resolve() answers when it finds no domain");

    // assign() counts before it answers, so neither of the two above needs waiting out.
    ctx.checkf(working.readCount(cmed::observe::WorkerEvent::Handed) == 0,
               "and neither was handed to a worker, at %" PRIu64,
               working.readCount(cmed::observe::WorkerEvent::Handed));
    ctx.check(!working.isBusy(cmed::MaxDomains), "the id reads as not busy");
    ctx.checkf(working.busy().takeSnapshot().getWord(0) == 0, "and no bit went up, at mask %" PRIx64,
               working.busy().takeSnapshot().getWord(0));
}

void aScopeLeftByAThrowStopsAndJoins(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a pool left by a throw");

    constexpr std::uint32_t Workers = 8;

    // Well under the fallback each worker's own wait times out on, so a stop that reached its workers
    // only by outliving that timeout would fail this rather than pass it slowly.
    constexpr timing::Millis JoinBudget{30};

    // Outside the scope the throw leaves, so what a serve wrote is still readable afterwards.
    ServedDomains serving{scratch, 1};
    timing::Stopwatch leaving;
    timing::Nanos unwound{0};

    // Reaching the check below at all is half the case: a std::thread destroyed without a join ends
    // the process, and this probe would report nothing rather than a failure.
    try
    {
        serving.askForLock(FirstSlot);
        cmed::daemon::DomainWorkers working{serving.domains(), Workers, NoSpin};
        ctx.check(awaitAssign(working, FirstSlot), "a domain is handed out before the throw");
        ctx.check(serving.awaitHeld(FirstSlot), "and its serve answers the request");

        leaving.restart();
        throw LeavingTheScope_t{};
    }
    catch (const LeavingTheScope_t&)
    {
        // Read here rather than after the handler, so the span is the unwind and not the checks below.
        unwound = leaving.elapsed();
    }
    ctx.checkf(unwound < JoinBudget,
               "the unwind stopped and joined %" PRIu32 " workers in %" PRId64 " ms", Workers,
               static_cast<std::int64_t>(timing::getTicks<timing::Millis>(unwound)));
    ctx.checkf(serving.counted(cmed::observe::DomainEvent::Grants) == 1,
               "and the pool served only what it was handed, at %" PRIu64 " grants",
               serving.counted(cmed::observe::DomainEvent::Grants));
}

// Deltas rather than totals for the two deferrals, because awaitAssign retries and each refused try
// is a real DeferredNoWorker. What each check pins is the count that must not have moved.
void theCountsOneHandOverMoves(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("the counts a hand-over moves, and the ones it does not");

    constexpr std::uint32_t Workers = 2;

    ServedDomains serving{scratch, 3};
    serving.holdEveryTurn();
    serving.askForLock(FirstSlot);
    serving.askForLock(SecondSlot);

    cmed::daemon::DomainWorkers working{serving.domains(), Workers, NoSpin};

    const auto reads = [&working](cmed::observe::WorkerEvent asked)
    {
        return working.readCount(asked);
    };

    // Before any hand-over. A set that starts at anything but zero would make every reading below it
    // a difference from a number nobody wrote.
    ctx.check(reads(cmed::observe::WorkerEvent::Handed) == 0 &&
                  reads(cmed::observe::WorkerEvent::DeferredBusy) == 0 &&
                  reads(cmed::observe::WorkerEvent::DeferredNoWorker) == 0 &&
                  reads(cmed::observe::WorkerEvent::Woke) == 0 &&
                  reads(cmed::observe::WorkerEvent::SkippedWake) == 0,
              "a fresh pool reads zero for every event it counts");

    if (!ctx.check(awaitAssign(working, FirstSlot) && serving.awaitGranting(1),
                   "one domain is handed over and its serve is inside"))
    {
        serving.letTurnsGo();
        return;
    }

    ctx.checkf(reads(cmed::observe::WorkerEvent::Handed) == 1, "one hand-over is one Handed, at %" PRIu64,
               reads(cmed::observe::WorkerEvent::Handed));
    ctx.check(reads(cmed::observe::WorkerEvent::Woke) + reads(cmed::observe::WorkerEvent::SkippedWake) == 1,
              "and exactly one of the two wake paths, since a hand-over takes one or neither");
    ctx.check(reads(cmed::observe::WorkerEvent::DeferredBusy) == 0,
              "with nothing counted as deferred for a busy domain");

    // The refusal that is about the domain. Its own count, because the fix for a run full of these is
    // not the fix for a run full of the other one.
    const std::uint64_t noWorkerBefore = reads(cmed::observe::WorkerEvent::DeferredNoWorker);
    ctx.check(!working.assign(FirstSlot), "a second hand-over of that same domain is refused");
    ctx.checkf(reads(cmed::observe::WorkerEvent::DeferredBusy) == 1, "counted once as busy, at %" PRIu64,
               reads(cmed::observe::WorkerEvent::DeferredBusy));
    ctx.check(reads(cmed::observe::WorkerEvent::Handed) == 1 &&
                  reads(cmed::observe::WorkerEvent::DeferredNoWorker) == noWorkerBefore,
              "and neither the hand-over count nor the other deferral moved with it");

    // The refusal that is about the pool. Both workers are inside a serve, so the third domain has
    // nowhere to go and the reason is a different one.
    if (!ctx.check(awaitAssign(working, SecondSlot) && serving.awaitGranting(Workers),
                   "the second worker takes a domain too, so both are at work"))
    {
        serving.letTurnsGo();
        return;
    }

    const std::uint64_t busyBefore = reads(cmed::observe::WorkerEvent::DeferredBusy);
    const std::uint64_t noWorkerNow = reads(cmed::observe::WorkerEvent::DeferredNoWorker);
    ctx.check(!working.assign(ThirdSlot), "a third domain is refused with every worker at work");
    ctx.check(reads(cmed::observe::WorkerEvent::DeferredNoWorker) == noWorkerNow + 1,
              "counted once as having no worker to give it to");
    ctx.check(reads(cmed::observe::WorkerEvent::DeferredBusy) == busyBefore &&
                  reads(cmed::observe::WorkerEvent::Handed) == 2,
              "and the busy deferral and the hand-over count stayed where they were");

    serving.letTurnsGo();
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"workers-probe"};
    return probe::run("workers probe",
                      [&scratch](probe::Context& ctx)
                      {
                          aCountTheIdleWordCannotHoldIsRefused(ctx, scratch);
                          aDomainReachesTheWorkerItWasHandedTo(ctx, scratch);
                          aDomainAlreadyWithAWorkerIsRefused(ctx, scratch);
                          aPoolWithNoIdleWorkerIsRefused(ctx, scratch);
                          theCountsOneHandOverMoves(ctx, scratch);
                          anIdPastTheTableIsNotAHandOver(ctx, scratch);
                          aScopeLeftByAThrowStopsAndJoins(ctx, scratch);
                      });
}
