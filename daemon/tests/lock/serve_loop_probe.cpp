// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// serve_loop_probe.cpp -- the turn a daemon takes until it is told to stop, and the two threads it
// takes it on.
//
// Each claim here fails as a slow daemon rather than as a broken one: a loop that never reaches the
// top of its turn, a maintenance thread taking the one wake a knock sends, a throw that leaves a
// thread running. So every case measures a span against the idle turn it was given.

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cme/shared.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "daemon/domain/manager.hpp"
#include "daemon/observe/counters.hpp"
#include "daemon/serve/handler.hpp"
#include "daemon/startup/config.hpp"
#include "harness/helper.hpp"
#include "harness/helper_cme_region.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

// Straight to the kernel with no spin ahead of it, so the parked flag a case waits on is raised as
// soon as the loop has nothing to serve.
constexpr timing::Micros NoSpin{0};

constexpr std::uint32_t FirstSlot = cmed::harness::FirstDataSlot;
constexpr std::uint32_t SecondSlot = FirstSlot + 1;
constexpr std::uint32_t ThirdSlot = FirstSlot + 2;

// The control domain and the three data slots the cases name.
constexpr std::uint32_t RegionSlots = 4;

// Far past any case, so this daemon keeps the turn it bought and no sweep takes one back under a case.
constexpr timing::Millis CohortHold{10000};

// Short enough that no case waits on it, for the cases whose subject is not the sleep.
constexpr timing::Millis BriefTurn{50};

[[nodiscard]] std::uint64_t bitFor(std::uint32_t domainId) noexcept
{
    return std::uint64_t{1} << domainId;
}

[[nodiscard]] std::int64_t inMillis(timing::Nanos span) noexcept
{
    return static_cast<std::int64_t>(timing::getTicks<timing::Millis>(span));
}

// The flag the doorbell wait raises for as long as it is queued in the kernel. A case that set a
// pending bit before the loop was asleep would be measuring the pass it interrupted.
[[nodiscard]] bool awaitParked(const cmed::protocol::SharedArea_t& area)
{
    return poll::waitUntil(
        [&area]
        {
            return area.isDispatcherParked();
        },
        cmed::harness::ProbeAttachWait, cmed::harness::ProbePoll);
}

// Thrown to leave a scope the way a failure would. What is under test is the destructor a throw
// reaches, so what the exception carries does not matter.
struct LeavingTheScope_t
{
};

// A tick past every case, so the sweep a build runs at once is the only one a case sees.
constexpr timing::Millis QuietTick{60000};

// What a ServeHandler is built from, naming only the settings a case varies. Built here rather than
// read from a file, since nothing else a deployment would write changes what either pass does.
[[nodiscard]] cmed::daemon::DaemonConfig_t handlerConfig(timing::Millis idleTurn,
                                                         timing::Millis tick = QuietTick,
                                                         std::uint32_t workerCount = 1,
                                                         timing::Micros spin = NoSpin)
{
    cmed::daemon::DaemonConfig_t config;
    config.serve.idleInterval = idleTurn;
    config.serve.spin = spin;
    config.maintenance.interval = tick;
    config.workers.count = workerCount;
    config.workers.spin = NoSpin;
    return config;
}

// A refresh on every maintenance pass rather than on an interval of its own, so a pass a case counts
// is one that read the region rather than one that skipped it.
[[nodiscard]] cmed::daemon::DaemonConfig_t configForServing()
{
    cmed::daemon::DaemonConfig_t config;
    config.cohort.hold = CohortHold;
    config.registry.refreshInterval = timing::Millis::zero();
    return config;
}

// Both halves of a domain, plus a second peer that can hold the region's turn for one. A worker
// granting a domain whose turn that peer holds waits inside the acquire, which keeps it in the busy mask.
class ServedDomains
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // @domainCount data domains from FirstSlot upwards, created in the region and published in the
    // area at the same ids: a create takes the lowest free record, so the two sides agree.
    ServedDomains(cmed::harness::ProbeScratch& scratch, std::uint32_t domainCount)
        : domainCount_{domainCount},
          area_{"serve-loop-probe"},
          config_{configForServing()},
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
    void holdTurn(std::uint32_t domainId)
    {
        turns_.push_back(contender_.lock(nameFor(domainId)));
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
    [[nodiscard]] bool awaitGranting(std::uint32_t domainId)
    {
        return awaitState(domainId, cmed::protocol::RequestState::Granting, cmed::harness::ProbeAttachWait);
    }

    // @within is a parameter because one case waits for an answer and another waits to show none came.
    [[nodiscard]] bool awaitHeld(std::uint32_t domainId, timing::Millis within = cmed::harness::ProbeAttachWait)
    {
        return awaitState(domainId, cmed::protocol::RequestState::LockHeld, within);
    }

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] cmed::protocol::SharedArea_t& shared() noexcept
    {
        return area_.shared();
    }

    [[nodiscard]] cmed::daemon::DomainManager& domains() noexcept
    {
        return domains_;
    }

    [[nodiscard]] std::uint64_t counted(cmed::observe::DomainEvent asked) const
    {
        return domains_.readCount(asked);
    }

private:
    [[nodiscard]] static std::string nameFor(std::uint32_t domainId)
    {
        return "served-" + std::to_string(domainId);
    }

    [[nodiscard]] bool awaitState(std::uint32_t domainId, cmed::protocol::RequestState wanted,
                                  timing::Millis within)
    {
        return poll::waitUntil(
            [this, domainId, wanted]
            {
                return cmed::harness::readState(area_.shared(), domainId) == wanted;
            },
            within, cmed::harness::ProbePoll);
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

// Held by a case whose subject is what a stop costs, so the destruction that joins both threads
// happens at a moment the case picks rather than at the end of its scope.
using ServingHalf = std::optional<cmed::daemon::ServeHandler>;

// The count moves after the worker it names has already been woken, so a case that waited on that
// worker's own answer can still read the count before it has moved.
[[nodiscard]] bool awaitHandOvers(const cmed::daemon::ServeHandler& side, std::uint64_t wanted,
                                  timing::Millis within = cmed::harness::ProbeAttachWait)
{
    return poll::waitUntil(
        [&side, wanted]
        {
            return side.readCount(cmed::observe::WorkerEvent::Handed) >= wanted;
        },
        within, cmed::harness::ProbePoll);
}

void everyDomainWithWorkReachesAServeOnce(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("every domain with work, once each");

    ServedDomains serving{scratch, 3};
    const cmed::daemon::ServeHandler side{serving.shared(), serving.domains(), handlerConfig(BriefTurn)};

    serving.askForLock(FirstSlot);
    serving.askForLock(SecondSlot);
    serving.askForLock(ThirdSlot);

    serving.shared().ring(FirstSlot);
    serving.shared().ring(SecondSlot);
    serving.shared().ring(ThirdSlot);

    std::uint32_t answered = 0;
    answered += serving.awaitHeld(FirstSlot) ? 1U : 0U;
    answered += serving.awaitHeld(SecondSlot) ? 1U : 0U;
    answered += serving.awaitHeld(ThirdSlot) ? 1U : 0U;

    ctx.checkf(answered == 3, "three domains with work are answered, and %" PRIu32 " of them were", answered);
    ctx.checkf(serving.counted(cmed::observe::DomainEvent::Grants) == 3,
               "each of them once, since several rings for one domain are one bit, at %" PRIu64 " grants",
               serving.counted(cmed::observe::DomainEvent::Grants));
    ctx.check(awaitHandOvers(side, 3), "with a hand-over for each of them");
    ctx.checkf(side.readCount(cmed::observe::WorkerEvent::Handed) == 3,
               "and no fourth, at %" PRIu64 " hand-overs",
               side.readCount(cmed::observe::WorkerEvent::Handed));
    ctx.checkf(cmed::harness::getEveryPendingBit(serving.shared()) == 0,
               "and the bitmap is left with nothing up, at %" PRIx64,
               cmed::harness::getEveryPendingBit(serving.shared()));
}

void aDomainTheBusyMaskNamesIsLeftForItsWorker(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a domain the busy mask names");

    // A pass that leaves a bit up spins its window instead of sleeping, because the worker holding
    // that domain moves no word when it finishes. So this turn's spin is what bounds each pass.
    constexpr timing::Micros DeferredSpin{200};

    // What a wait may cost while a worker sits inside its acquire. Each pass comes back within the
    // spin above, and a longer wait would expire the acquire this case is holding open.
    constexpr timing::Millis WhileTheTurnIsHeld{100};

    // Two workers, so what leaves the second domain alone is its own bit in the mask rather than a
    // pool with nothing left to give.
    ServedDomains serving{scratch, 2};
    serving.holdTurn(SecondSlot);
    serving.askForLock(FirstSlot);
    serving.askForLock(SecondSlot);

    const cmed::daemon::ServeHandler side{serving.shared(), serving.domains(),
                                          handlerConfig(BriefTurn, QuietTick, 2, DeferredSpin)};

    serving.shared().ring(SecondSlot);
    if (!ctx.check(serving.awaitGranting(SecondSlot), "a worker is inside the serve for one domain"))
    {
        serving.letTurnsGo();
        return;
    }

    // Both, so the pass that reads them has one domain it may hand out and one it may not.
    serving.shared().ring(SecondSlot);
    serving.shared().ring(FirstSlot);

    ctx.check(serving.awaitHeld(FirstSlot), "the domain no worker holds is served by the same pass");
    ctx.checkf((cmed::harness::getEveryPendingBit(serving.shared()) & bitFor(SecondSlot)) != 0,
               "the one the mask names keeps its bit, at mask %" PRIx64,
               cmed::harness::getEveryPendingBit(serving.shared()));
    ctx.check(awaitHandOvers(side, 2, WhileTheTurnIsHeld), "which cost a hand-over of its own");
    ctx.checkf(side.readCount(cmed::observe::WorkerEvent::Handed) == 2,
               "and reaches no second worker, at %" PRIu64 " hand-overs",
               side.readCount(cmed::observe::WorkerEvent::Handed));

    // A pass that ends deferred saw a bit it could not hand out. The refusal count below reads zero
    // just as well for a pass that never looked, so the two are asserted together.
    ctx.check(poll::waitUntil(
                  [&side]
                  {
                      return side.readCount(cmed::observe::DispatchEvent::Deferred) >= 1;
                  },
                  WhileTheTurnIsHeld, cmed::harness::ProbePoll),
              "a pass ends deferred rather than idle, so one did reach the bit it left up");
    ctx.checkf(side.readCount(cmed::observe::WorkerEvent::DeferredBusy) == 0,
               "since the pass never offered it to the pool at all, at %" PRIu64 " busy refusals",
               side.readCount(cmed::observe::WorkerEvent::DeferredBusy));

    // Let go while the loop turns, since a worker finishing is what this stands for and nothing
    // announces either.
    serving.letTurnsGo();
    ctx.check(serving.awaitHeld(SecondSlot), "a later pass serves it once the mask lets go");
    ctx.check(awaitHandOvers(side, 3), "on a hand-over of its own");
    ctx.checkf(side.readCount(cmed::observe::WorkerEvent::Handed) == 3,
               "and on no further one, at %" PRIu64 " hand-overs",
               side.readCount(cmed::observe::WorkerEvent::Handed));

    ctx.checkf(cmed::harness::getEveryPendingBit(serving.shared()) == 0,
               "and the bitmap is left with nothing up, at %" PRIx64,
               cmed::harness::getEveryPendingBit(serving.shared()));
}

void aStopEndsTheTurnFromAnotherThread(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a stop against a turn that is asleep");

    constexpr timing::Millis LongTurn{2000};
    constexpr timing::Millis StopBudget{200};

    ServedDomains serving{scratch, 1};

    // A tick the maintenance thread would leave on its own well inside the budget, so what the span
    // below answers for is the dispatcher's doorbell and not both.
    ServingHalf side{std::in_place, serving.shared(), serving.domains(), handlerConfig(LongTurn, BriefTurn)};
    ctx.check(awaitParked(serving.shared()), "the loop reaches its doorbell sleep with nothing to serve");

    const timing::Stopwatch stopping;
    side.reset();
    const timing::Nanos ended = stopping.elapsed();

    ctx.checkf(ended < StopBudget, "a stop from another thread ends a %" PRId64 " ms sleep in %" PRId64 " ms",
               static_cast<std::int64_t>(LongTurn.count()), inMillis(ended));
    ctx.checkf(serving.counted(cmed::observe::DomainEvent::Grants) == 0,
               "and nothing was served, so it was the stop and not work, at %" PRIu64 " grants",
               serving.counted(cmed::observe::DomainEvent::Grants));
}

void theIdleTurnBoundsOneSleep(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("work the doorbell was never rung for");

    constexpr timing::Millis ShortTurn{100};
    constexpr timing::Millis ShortTurnBudget{600};
    constexpr timing::Millis LongTurn{3000};
    constexpr timing::Millis NotWithin{300};

    // A bit set with no knock. The doorbell cannot announce a peer that died holding a turn, so what
    // has to reach that domain is the sleep running out.
    {
        ServedDomains serving{scratch, 1};
        const cmed::daemon::ServeHandler side{serving.shared(), serving.domains(), handlerConfig(ShortTurn)};

        ctx.check(awaitParked(serving.shared()), "the loop is asleep with nothing pending");

        serving.askForLock(FirstSlot);
        const timing::Stopwatch waited;
        static_cast<void>(cmed::harness::setPendingBit(serving.shared(), FirstSlot));
        const bool served = serving.awaitHeld(FirstSlot, ShortTurnBudget);
        const timing::Nanos took = waited.elapsed();

        ctx.checkf(served, "a bit set with no knock is served in %" PRId64 " ms, against a %" PRId64 " ms idle turn",
                   inMillis(took), static_cast<std::int64_t>(ShortTurn.count()));
    }

    // The same bit against a turn long enough to say the wait is a sleep and not a look. Without this
    // the case above would pass just as well on a loop that never waited at all.
    {
        ServedDomains serving{scratch, 1};

        // The bit is read below rather than here, once both passes have stopped: what it stands for is
        // what a daemon leaves behind, and a pass still turning could take it mid-read.
        bool served = false;
        {
            const cmed::daemon::ServeHandler side{serving.shared(), serving.domains(), handlerConfig(LongTurn)};

            ctx.check(awaitParked(serving.shared()), "the loop is asleep with nothing pending");

            serving.askForLock(FirstSlot);
            static_cast<void>(cmed::harness::setPendingBit(serving.shared(), FirstSlot));

            served = serving.awaitHeld(FirstSlot, NotWithin);
        }

        ctx.checkf(!served, "against a %" PRId64 " ms turn the same bit waits out %" PRId64 " ms unserved",
                   static_cast<std::int64_t>(LongTurn.count()), static_cast<std::int64_t>(NotWithin.count()));
        ctx.check((cmed::harness::getEveryPendingBit(serving.shared()) & bitFor(FirstSlot)) != 0,
                  "and the bit is still up for the next daemon rather than dropped");
    }
}

void theMaintenancePassRunsOnItsOwnTick(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("the maintenance pass and its tick");

    constexpr timing::Millis LongTick{2000};
    constexpr timing::Millis FirstPassBudget{300};
    constexpr timing::Millis StopBudget{300};
    constexpr timing::Millis ShortTick{20};
    constexpr std::uint32_t Passes = 5;

    // A pass runs at the top of the turn, before the first wait, so the first one costs no tick at
    // all. Against a tick this long, a pass that waited first would not arrive inside the budget.
    {
        ServedDomains serving{scratch, 1};
        ServingHalf side{std::in_place, serving.shared(), serving.domains(), handlerConfig(BriefTurn, LongTick)};

        const bool ranAtOnce = poll::waitUntil(
            [&side]
            {
                return side->readCount(cmed::observe::MaintainEvent::Passes) >= 1;
            },
            FirstPassBudget, cmed::harness::ProbePoll);

        const timing::Stopwatch stopping;
        side.reset();
        const timing::Nanos ended = stopping.elapsed();

        ctx.checkf(ranAtOnce, "a pass runs within %" PRId64 " ms of the thread starting, against a %" PRId64 " ms tick",
                   static_cast<std::int64_t>(FirstPassBudget.count()),
                   static_cast<std::int64_t>(LongTick.count()));
        ctx.checkf(ended < StopBudget, "and a stop ends that tick in %" PRId64 " ms, so it moves this thread's own word too",
                   inMillis(ended));
    }

    // The tick itself. A thread that ran its pass back to back would reach the count in microseconds,
    // so the span and not the count is the assertion.
    {
        ServedDomains serving{scratch, 1};

        const timing::Stopwatch running;
        const cmed::daemon::ServeHandler side{serving.shared(), serving.domains(),
                                              handlerConfig(BriefTurn, ShortTick)};

        const bool reached = poll::waitUntil(
            [&side]
            {
                return side.readCount(cmed::observe::MaintainEvent::Passes) >= Passes;
            },
            cmed::harness::ProbeAttachWait, cmed::harness::ProbePoll);
        const timing::Nanos spent = running.elapsed();

        const timing::Millis least = ShortTick * static_cast<timing::Millis::rep>(Passes - 1);
        ctx.checkf(reached, "%" PRIu64 " passes run, of the %" PRIu32 " asked for",
                   side.readCount(cmed::observe::MaintainEvent::Passes), Passes);
        ctx.checkf(spent >= least, "and take %" PRId64 " ms, at least the %" PRId64 " ms of tick between them",
                   inMillis(spent), static_cast<std::int64_t>(least.count()));
    }
}

void theMaintenanceThreadDoesNotTakeTheDoorbellsWake(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a knock with a maintenance thread also asleep");

    constexpr std::uint32_t Rounds = 40;
    constexpr timing::Millis LongTurn{500};
    constexpr timing::Millis LongTick{500};
    constexpr timing::Millis Budget{100};
    constexpr timing::Millis BetweenRounds{2};

    // A knock wakes one waiter. Were the maintenance thread queued on the doorbell it would take
    // roughly half of these, and each pass it woke for would be a knock the dispatcher slept through.
    constexpr std::uint64_t MostPasses = 5;

    ServedDomains serving{scratch, 1};
    const cmed::daemon::ServeHandler side{serving.shared(), serving.domains(), handlerConfig(LongTurn, LongTick)};
    ctx.check(awaitParked(serving.shared()), "the dispatcher is inside its doorbell sleep");

    timing::Nanos worst{0};
    std::uint32_t answered = 0;

    for (std::uint32_t round = 0; round < Rounds; ++round)
    {
        serving.askForLock(FirstSlot);

        const timing::Stopwatch waited;
        serving.shared().ring(FirstSlot);
        answered += serving.awaitHeld(FirstSlot) ? 1U : 0U;

        worst = std::max(worst, waited.elapsed());
        std::this_thread::sleep_for(BetweenRounds);
    }

    // Read while both threads still turn, since what it answers for is the rounds above and a later
    // tick would only add to it.
    const std::uint64_t passes = side.readCount(cmed::observe::MaintainEvent::Passes);

    ctx.checkf(answered == Rounds, "%" PRIu32 " of %" PRIu32 " knocks were served", answered, Rounds);
    ctx.checkf(worst < Budget, "the slowest took %" PRId64 " ms, against a %" PRId64 " ms idle turn",
               inMillis(worst), static_cast<std::int64_t>(LongTurn.count()));
    ctx.checkf(passes <= MostPasses, "and the maintenance pass ran %" PRIu64 " times across those knocks", passes);
}

void bothThreadsAreJoinedOnTheWayOutOfAScope(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a scope left by a throw with both threads turning");

    constexpr timing::Millis LongTurn{3000};
    constexpr timing::Millis LongTick{3000};
    constexpr timing::Millis JoinBudget{300};
    constexpr timing::Millis FirstPassBudget{300};

    ServedDomains serving{scratch, 1};

    timing::Stopwatch leaving;
    timing::Nanos unwound{0};

    // Reaching the checks below at all is half the case: a std::thread destroyed without a join ends
    // the process, and this probe would then report nothing rather than a failure.
    try
    {
        const cmed::daemon::ServeHandler side{serving.shared(), serving.domains(),
                                              handlerConfig(LongTurn, LongTick)};
        ctx.check(awaitParked(serving.shared()), "the dispatcher reaches its sleep");
        ctx.check(poll::waitUntil([&side]
                                  {
                                      return side.readCount(cmed::observe::MaintainEvent::Passes) >= 1;
                                  },
                                  FirstPassBudget, cmed::harness::ProbePoll),
                  "and the maintenance thread has run its pass and is waiting out its tick");

        leaving.restart();
        throw LeavingTheScope_t{};
    }
    catch (const LeavingTheScope_t&)
    {
        // Read here rather than after the handler, so the span is the unwind and not the checks below.
        unwound = leaving.elapsed();
    }
    ctx.checkf(unwound < JoinBudget,
               "the unwind stopped the loop and joined both threads in %" PRId64 " ms, against a %" PRId64 " ms turn and the same tick",
               inMillis(unwound), static_cast<std::int64_t>(LongTurn.count()));
    ctx.checkf(serving.counted(cmed::observe::DomainEvent::Grants) == 0,
               "with nothing served, so neither thread was ended by work, at %" PRIu64 " grants",
               serving.counted(cmed::observe::DomainEvent::Grants));
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"serve-loop-probe"};
    return probe::run("serve loop probe",
                      [&scratch](probe::Context& ctx)
                      {
                          everyDomainWithWorkReachesAServeOnce(ctx, scratch);
                          aDomainTheBusyMaskNamesIsLeftForItsWorker(ctx, scratch);
                          aStopEndsTheTurnFromAnotherThread(ctx, scratch);
                          theIdleTurnBoundsOneSleep(ctx, scratch);
                          theMaintenancePassRunsOnItsOwnTick(ctx, scratch);
                          theMaintenanceThreadDoesNotTakeTheDoorbellsWake(ctx, scratch);
                          bothThreadsAreJoinedOnTheWayOutOfAScope(ctx, scratch);
                      });
}
