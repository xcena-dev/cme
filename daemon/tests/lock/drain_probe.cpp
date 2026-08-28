// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// drain_probe.cpp -- the two rules the drain exists for.
//
// Every other probe turns a pass without racing the drain, so neither rule shows in a normal run.
// Both fail silently when broken: a lost request looks like a slow one, and a missed wake looks like
// a timeout. What a domain reached is read off its slot rather than off a counter of calls, because
// a serve is what moves that slot and nothing else does.

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <string>
#include <thread>
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
#include "shared/util/futex.hpp"

namespace
{

constexpr std::uint32_t FirstSlot = cmed::harness::FirstDataSlot;
constexpr std::uint32_t SecondSlot = FirstSlot + 1;

// The control domain and the eight data slots the storm below rings.
constexpr std::uint32_t RegionSlots = 9;

// Far past any case, so this daemon keeps the turn it bought and a second grant costs no acquire.
constexpr timing::Millis CohortHold{10000};

[[nodiscard]] std::uint64_t bitFor(std::uint32_t domainId) noexcept
{
    return std::uint64_t{1} << domainId;
}

[[nodiscard]] cmed::daemon::DaemonConfig_t configWithHold(timing::Millis cohortHold)
{
    cmed::daemon::DaemonConfig_t config;
    config.cohort.hold = cohortHold;
    return config;
}

// Both halves of a domain, plus a second peer that can hold the region's turn for it. A worker
// granting a domain whose turn that peer holds waits inside the acquire until the turn is let go,
// which is the only way to keep a domain in flight while a case asks what the drain did.
class ServedDomains
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // @domainCount data domains from FirstSlot upwards, created in the region and published in the
    // area at the same ids: a create takes the lowest free record, so the two sides agree.
    ServedDomains(cmed::harness::ProbeScratch& scratch, std::uint32_t domainCount)
        : domainCount_{domainCount},
          area_{"drain-probe"},
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

    [[nodiscard]] bool isHeld(std::uint32_t domainId)
    {
        return cmed::harness::isHeld(area_.shared(), domainId);
    }

private:
    [[nodiscard]] static std::string nameFor(std::uint32_t domainId)
    {
        return "drained-" + std::to_string(domainId);
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

// A maintenance pace no case here reaches, so the sweep a build runs at once is the only one and no
// later one moves a word a case is reading.
constexpr timing::Millis QuietTick{60000};

// What a drainer is built from, over a pool of one worker: a refusal a case wants is then the pool
// running out rather than the domain already being busy.
[[nodiscard]] cmed::daemon::DaemonConfig_t drainConfig(timing::Millis idleTurn, timing::Micros spin)
{
    cmed::daemon::DaemonConfig_t config;
    config.serve.idleInterval = idleTurn;
    config.serve.spin = spin;
    config.maintenance.interval = QuietTick;
    config.workers.count = 1;
    config.workers.spin = timing::Micros{0};
    return config;
}

// The spin a deployment ships with, taken from the struct rather than written down twice.
[[nodiscard]] cmed::daemon::DaemonConfig_t drainConfig(timing::Millis idleTurn)
{
    return drainConfig(idleTurn, cmed::daemon::DaemonConfig_t{}.serve.spin);
}

// A pass that took a bit and found no worker for it, which is what a case needs before it may read
// what that pass left up. Nothing announces a pass, so this count is what says one happened.
[[nodiscard]] bool awaitNoWorkerRefusal(const cmed::daemon::ServeHandler& drainer)
{
    return poll::waitUntil(
        [&drainer]
        {
            return drainer.readCount(cmed::observe::WorkerEvent::DeferredNoWorker) >= 1;
        },
        cmed::harness::ProbeAttachWait, cmed::harness::ProbePoll);
}

[[nodiscard]] bool awaitHandOvers(const cmed::daemon::ServeHandler& drainer, std::uint64_t wanted)
{
    return poll::waitUntil(
        [&drainer, wanted]
        {
            return drainer.readCount(cmed::observe::WorkerEvent::Handed) >= wanted;
        },
        cmed::harness::ProbeAttachWait, cmed::harness::ProbePoll);
}

// The bits a pass takes are cleared before any worker runs, so a raise landing after that clear is
// carried by the next pass. Scan, serve, then clear would wipe this raise instead.
void aRaiseMadeWhileItsOwnServeRunsSurvives(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a bit raised while its own serve is running");

    ServedDomains serving{scratch, 1};
    serving.holdEveryTurn();
    serving.askForLock(FirstSlot);

    {
        const cmed::daemon::ServeHandler drainer{serving.shared(), serving.domains(),
                                                 drainConfig(timing::Millis{5})};

        serving.shared().ring(FirstSlot);
        if (!ctx.check(serving.awaitGranting(FirstSlot), "the one worker is inside the serve for that domain"))
        {
            serving.letTurnsGo();
            return;
        }

        // Raised after the pass that took the first bit had already cleared it, and while the worker
        // it handed the domain to is still inside.
        serving.shared().ring(FirstSlot);
        ctx.checkf((cmed::harness::getEveryPendingBit(serving.shared()) & bitFor(FirstSlot)) != 0,
                   "its bit stands rather than being cleared by the pass in flight, at mask %" PRIx64,
                   cmed::harness::getEveryPendingBit(serving.shared()));

        serving.letTurnsGo();
        ctx.check(serving.awaitHeld(FirstSlot), "the serve answers the request once the turn is free");
        ctx.check(awaitHandOvers(drainer, 2), "and the raise made mid-serve reaches a serve of its own");
        ctx.checkf(drainer.readCount(cmed::observe::WorkerEvent::Handed) == 2,
                   "which is two hand-overs for one domain, at %" PRIu64,
                   drainer.readCount(cmed::observe::WorkerEvent::Handed));
    }

    ctx.checkf(cmed::harness::getEveryPendingBit(serving.shared()) == 0,
               "and the bitmap is left with nothing up, at %" PRIx64,
               cmed::harness::getEveryPendingBit(serving.shared()));
}

// Raises for one domain coalesce into one bit, so a storm of them costs far fewer grants than raises.
// What the drain owes is that the last raise for each domain still reaches a serve.
void noDomainIsLeftUnservedByAStorm(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("raisers against a drainer");

    constexpr std::uint32_t Raisers = 4;
    constexpr std::uint32_t RaisesEach = 4000;
    constexpr std::uint32_t Domains = 8;
    constexpr std::uint64_t Raises = std::uint64_t{Raisers} * RaisesEach;

    ServedDomains serving{scratch, Domains};

    {
        const cmed::daemon::ServeHandler drainer{serving.shared(), serving.domains(),
                                                 drainConfig(timing::Millis{1})};

        std::vector<std::thread> raisers;
        raisers.reserve(Raisers);
        for (std::uint32_t index = 0; index < Raisers; ++index)
        {
            raisers.emplace_back(
                [&serving, index]
                {
                    // A cheap xorshift rather than <random>: what matters is only that the domains
                    // and the gaps are not in lockstep with the drainer.
                    std::uint32_t state = index + 1;
                    for (std::uint32_t round = 0; round < RaisesEach; ++round)
                    {
                        state ^= state << 13U;
                        state ^= state >> 17U;
                        state ^= state << 5U;

                        // The request goes up before the knock. Rung first, a pass could read the state
                        // the previous round left and answer nothing at all for this raise.
                        const std::uint32_t domainId = FirstSlot + (state % Domains);
                        serving.askForLock(domainId);
                        serving.shared().ring(domainId);
                    }
                });
        }
        for (std::thread& raiser : raisers)
        {
            raiser.join();
        }

        // Quiescence, not a fixed sleep: the last raises are still in flight when the raisers exit.
        std::uint32_t answered = 0;
        for (std::uint32_t domainId = FirstSlot; domainId < FirstSlot + Domains; ++domainId)
        {
            answered += serving.awaitHeld(domainId) ? 1U : 0U;
        }

        ctx.checkf(answered == Domains, "%" PRIu32 " of %" PRIu32 " domains end answered", answered, Domains);
    }

    ctx.checkf(cmed::harness::getEveryPendingBit(serving.shared()) == 0,
               "with nothing left up for a pass that will not come, at %" PRIx64,
               cmed::harness::getEveryPendingBit(serving.shared()));

    const std::uint64_t grants = serving.counted(cmed::observe::DomainEvent::Grants);
    ctx.checkf(grants >= Domains, "%" PRIu64 " grants answered %" PRIu64 " raises", grants, Raises);
    ctx.checkf(grants < Raises, "which is fewer than the raises, since raises for one domain are one bit");
}

// The mechanism the doorbell rule rests on: FUTEX_WAIT compares the word under the kernel's queue lock,
// so a value captured before the raise stops the sleep and one captured after it does not.
void theCapturedValueDecidesWhetherTheWaitSleeps(probe::Context& ctx, cmed::protocol::SharedArea_t& area)
{
    ctx.openCase("the value a wait compares against");

    constexpr timing::Millis Nap{300};

    {
        // Captured before the raise, which is a pass's own order.
        const std::uint32_t seen = cmed::harness::getDoorbell(area);
        area.ring(FirstSlot);

        const timing::Stopwatch waited;
        static_cast<void>(cmed::util::waitOnWord(cmed::harness::resolveDoorbellWord(area), seen, Nap));
        ctx.check(waited.elapsed() < Nap / 2, "a value captured before the raise does not sleep");
    }

    cmed::harness::clearEveryPendingBit(area);

    {
        // Captured after the raise, which is what reading the doorbell late would do.
        area.ring(FirstSlot);
        const std::uint32_t seen = cmed::harness::getDoorbell(area);

        const timing::Stopwatch waited;
        static_cast<void>(cmed::util::waitOnWord(cmed::harness::resolveDoorbellWord(area), seen, Nap));
        ctx.check(waited.elapsed() >= Nap, "and a value captured after it sleeps through the raise");
    }

    cmed::harness::clearEveryPendingBit(area);
}

// The promptness the doorbell rule buys. A wait that ever started against a value already carrying
// the new work would cost the whole idle turn, so the slowest round trip is the assertion.
void aRaiseIsServedWithoutWaitingOutTheIdleTurn(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a raise against a long idle turn");

    constexpr std::uint32_t Rounds = 60;
    constexpr timing::Millis IdleTurn{500};
    constexpr timing::Millis Budget{100};

    ServedDomains serving{scratch, 1};
    timing::Nanos worst{0};
    std::uint32_t answered = 0;

    {
        const cmed::daemon::ServeHandler drainer{serving.shared(), serving.domains(), drainConfig(IdleTurn)};

        for (std::uint32_t round = 0; round < Rounds; ++round)
        {
            // The request first, so the state this round waits to see is one only this round's serve
            // could have written.
            serving.askForLock(FirstSlot);

            const timing::Stopwatch waited;
            serving.shared().ring(FirstSlot);
            answered += serving.awaitHeld(FirstSlot) ? 1U : 0U;

            worst = std::max(worst, waited.elapsed());
        }
    }

    const auto worstMs = timing::getTicks<timing::Millis>(worst);
    ctx.checkf(answered == Rounds, "%" PRIu32 " of %" PRIu32 " raises are answered", answered, Rounds);
    ctx.checkf(worst < Budget, "the slowest of them took %" PRId64 " ms, against a %" PRId64 " ms idle turn",
               static_cast<std::int64_t>(worstMs), static_cast<std::int64_t>(IdleTurn.count()));
}

// What a refused hand-over leaves behind. A pass that leaves either bit up may not sleep the turn out,
// since what frees a worker moves no word a wait could key on.
void aRefusedHandOverLeavesTheBitUp(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("the bit a refused hand-over leaves");

    constexpr timing::Millis IdleTurn{300};
    constexpr timing::Micros SpinFor{1000};

    // Half the turn. A pass that came back on its own serves within about the spin, so this separates
    // the two answers by a wide margin without asserting a number the scheduler has to hit.
    constexpr timing::Millis DeferredBudget{150};

    // What a hand-over that did happen would land well inside, so a request unanswered at the end of
    // it is one no worker took.
    constexpr timing::Millis NothingArrives{50};

    // Two domains whose turns a second peer holds. The first is in flight with the pool's one worker,
    // and the second has nowhere to go while that worker is inside its serve.
    ServedDomains serving{scratch, 2};
    serving.holdEveryTurn();
    serving.askForLock(FirstSlot);
    serving.askForLock(SecondSlot);

    cmed::harness::clearEveryPendingBit(serving.shared());

    {
        const cmed::daemon::ServeHandler drainer{serving.shared(), serving.domains(),
                                                 drainConfig(IdleTurn, SpinFor)};

        serving.shared().ring(FirstSlot);
        if (!ctx.check(serving.awaitGranting(FirstSlot), "the pool's one worker is inside the serve"))
        {
            serving.letTurnsGo();
            return;
        }

        static_cast<void>(cmed::harness::setPendingBit(serving.shared(), FirstSlot));
        static_cast<void>(cmed::harness::setPendingBit(serving.shared(), SecondSlot));

        // One knock for the two bits, because setting a bit rings nothing and the pass that went to
        // sleep read the doorbell before either went up. Read after the knock, so what the checks
        // below compare against is this case's own last ring.
        serving.shared().knock();
        const std::uint32_t doorbellBefore = serving.shared().readDoorbell();

        if (!ctx.check(awaitNoWorkerRefusal(drainer), "a pass takes the free bit and finds no worker for it"))
        {
            serving.letTurnsGo();
            return;
        }

        ctx.checkf((cmed::harness::getEveryPendingBit(serving.shared()) & bitFor(FirstSlot)) != 0,
                   "the domain already with a worker keeps its bit, at mask %" PRIu64,
                   cmed::harness::getEveryPendingBit(serving.shared()));

        // The pool is never asked about a domain the pass already knows a worker holds, so the refusal
        // that would name one stays at zero while the other one climbs.
        ctx.checkf(drainer.readCount(cmed::observe::WorkerEvent::DeferredBusy) == 0,
                   "which the pass never offered it at all, at %" PRIu64 " busy refusals",
                   drainer.readCount(cmed::observe::WorkerEvent::DeferredBusy));
        ctx.checkf((cmed::harness::getEveryPendingBit(serving.shared()) & bitFor(SecondSlot)) != 0,
                   "the one with no idle worker keeps its bit too, at mask %" PRIu64,
                   cmed::harness::getEveryPendingBit(serving.shared()));
        ctx.check(!serving.awaitHeld(SecondSlot, NothingArrives),
                  "and its request goes unanswered, so no worker took it");

        ctx.check(serving.shared().readDoorbell() == doorbellBefore,
                  "neither refusal rings the doorbell, since what frees the worker is that worker finishing");

        // The worker finishing is the mask clearing, and nothing announces it. A pass that slept the
        // turn out would serve what it left up to one idle turn later; one that came back on its own
        // serves it within about the spin.
        const timing::Stopwatch freed;
        serving.letTurnsGo();
        const bool taken = serving.awaitHeld(FirstSlot) && serving.awaitHeld(SecondSlot);
        const auto freedMs = timing::getTicks<timing::Millis>(freed.elapsed());

        ctx.check(taken, "and once the turns are free both of them are answered");
        ctx.checkf(freed.elapsed() < DeferredBudget,
                   "%" PRId64 " ms after they came free, against a %" PRId64 " ms idle turn",
                   static_cast<std::int64_t>(freedMs), static_cast<std::int64_t>(IdleTurn.count()));
    }

    cmed::harness::clearEveryPendingBit(serving.shared());
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"drain-probe"};
    return probe::run("drain probe",
                      [&scratch](probe::Context& ctx)
                      {
                          cmed::harness::ProbeArea area{"drain-probe"};

                          aRaiseMadeWhileItsOwnServeRunsSurvives(ctx, scratch);
                          noDomainIsLeftUnservedByAStorm(ctx, scratch);
                          theCapturedValueDecidesWhetherTheWaitSleeps(ctx, area.shared());
                          aRaiseIsServedWithoutWaitingOutTheIdleTurn(ctx, scratch);
                          aRefusedHandOverLeavesTheBitUp(ctx, scratch);
                      });
}
