// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// held_domains_probe.cpp -- what the daemon does with a request, and what it costs in CXL turns.
//
// serve() is driven directly, not through a ServeLoop, so a case sets the state it wants answered
// and reads the answer on the next line; the drain loop and doorbell are drain_probe's subject.
//
// The subject is cohorting, which is invisible in the state words, so every case reads the grant count
// against the acquire count. waiters is stored by hand rather than raised by a spawned requester.

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <string>

#include "common/timing.hpp"
#include "daemon/domain/manager.hpp"
#include "daemon/observe/counters.hpp"
#include "daemon/startup/config.hpp"
#include "harness/helper.hpp"
#include "harness/helper_cme_region.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

// Every case reads the same two words off the same manager, so the spelling stays one call here.
[[nodiscard]] std::uint64_t readCount(const cmed::daemon::DomainManager& domains, cmed::observe::DomainEvent asked)
{
    return domains.readCount(asked);
}

constexpr const char* DomainName = "held";

// Slot 0 is the control domain in both, so the data domain sits at 1 on each side. The two are
// unrelated ceilings that happen to agree.
constexpr std::uint32_t Slot = cmed::harness::FirstDataSlot;
constexpr std::uint32_t RegionSlots = 4;

// Long enough that no case here reaches it, so a run ends because the case ended it.
constexpr timing::Millis CohortHold{10000};

[[nodiscard]] cmed::daemon::DaemonConfig_t configWithHold(timing::Millis cohortHold)
{
    cmed::daemon::DaemonConfig_t config;
    config.cohort.hold = cohortHold;
    return config;
}

// One request, answered. Returns nothing: every case reads the state and the counters itself,
// because what each is asking about differs.
void request(cmed::protocol::SharedArea_t& area, cmed::daemon::DomainManager& domains,
             cmed::protocol::RequestState wanted)
{
    cmed::harness::resolveSlot(area, Slot).publish(wanted);
    domains.serveLock(Slot);
}

// A holder leaving. Idle rather than a release state of its own: a holder publishes that itself and only
// rings, so what reaches the daemon is a domain rung while idle.
void release(cmed::protocol::SharedArea_t& area, cmed::daemon::DomainManager& domains)
{
    request(area, domains, cmed::protocol::RequestState::Idle);
}

// Whether any turn stands behind this domain, asked the way a requester asks it. The hand-back is what
// makes this false, since the validity a grant is dated with outlasts a case by seconds.
[[nodiscard]] bool holdsATurn(cmed::protocol::SharedArea_t& area)
{
    return cmed::harness::resolveSlot(area, Slot).hasValidTurn();
}

void setQueued(cmed::protocol::SharedArea_t& area, std::uint32_t queued)
{
    cmed::harness::setWaiters(cmed::harness::resolveSlot(area, Slot), queued);
}

// ── cases ───────────────────────────────────────────────────────────────

void aGrantTakesOneTurn(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("one request, answered");

    const std::string areaName = scratch.makeAreaName("");
    const std::string regionName = scratch.makeAreaName("region");
    cmed::harness::ProbeArea area{areaName.c_str()};
    cmed::harness::ProbeRegion region{regionName.c_str(), RegionSlots};
    cmed::harness::publishDomain(area.shared(), Slot, DomainName);
    region.createDomain(DomainName);

    cmed::daemon::DomainManager domains{region.session(), area.shared(), configWithHold(CohortHold)};

    request(area.shared(), domains, cmed::protocol::RequestState::LockRequested);

    ctx.check(cmed::harness::isHeld(area.shared(), Slot), "the daemon publishes LockHeld");
    ctx.checkf(readCount(domains, cmed::observe::DomainEvent::Grants) == 1, "one grant, and it says %" PRIu64 "", readCount(domains, cmed::observe::DomainEvent::Grants));
    ctx.checkf(readCount(domains, cmed::observe::DomainEvent::Acquires) == 1, "one CXL turn taken for it, and it says %" PRIu64 "", readCount(domains, cmed::observe::DomainEvent::Acquires));

    ctx.check(holdsATurn(area.shared()), "and a turn stands behind the grant");

    // The acquire above is the one the mean is taken over, so a zero here says the clock reads never
    // ran rather than that the region answered instantly.
    ctx.checkf(domains.readMeanNanos(cmed::observe::SpanEvent::RemoteAcquire) > 0,
               "and what that turn cost was measured, at %" PRIu64 " ns",
               domains.readMeanNanos(cmed::observe::SpanEvent::RemoteAcquire));

    release(area.shared(), domains);

    ctx.check(!holdsATurn(area.shared()), "and the release hands that turn back");
}

void aQueuedWaiterKeepsTheTurn(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a second local requester rides the turn the first paid for");

    const std::string areaName = scratch.makeAreaName("");
    const std::string regionName = scratch.makeAreaName("region");
    cmed::harness::ProbeArea area{areaName.c_str()};
    cmed::harness::ProbeRegion region{regionName.c_str(), RegionSlots};
    cmed::harness::publishDomain(area.shared(), Slot, DomainName);
    region.createDomain(DomainName);

    cmed::daemon::DomainManager domains{region.session(), area.shared(), configWithHold(CohortHold)};

    request(area.shared(), domains, cmed::protocol::RequestState::LockRequested);

    // Someone is blocked on the domain mutex when the first requester lets go, which is the whole
    // condition: with nobody queued the turn would go back and the next grant would buy it again.
    setQueued(area.shared(), 1);
    release(area.shared(), domains);
    request(area.shared(), domains, cmed::protocol::RequestState::LockRequested);

    ctx.check(cmed::harness::isHeld(area.shared(), Slot), "the second requester is granted");
    ctx.checkf(readCount(domains, cmed::observe::DomainEvent::Grants) == 2, "two grants, and it says %" PRIu64 "", readCount(domains, cmed::observe::DomainEvent::Grants));
    ctx.checkf(readCount(domains, cmed::observe::DomainEvent::Acquires) == 1, "still one CXL turn behind them, and it says %" PRIu64 "", readCount(domains, cmed::observe::DomainEvent::Acquires));

    // Nobody left waiting, so this release is the one that hands the turn back.
    setQueued(area.shared(), 0);
    release(area.shared(), domains);
    request(area.shared(), domains, cmed::protocol::RequestState::LockRequested);

    ctx.checkf(readCount(domains, cmed::observe::DomainEvent::Acquires) == 2, "and the next grant buys a fresh turn, at %" PRIu64 "", readCount(domains, cmed::observe::DomainEvent::Acquires));
}

void theCohortHoldEndsTheRun(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a queued waiter does not keep the turn past the hold");

    const std::string areaName = scratch.makeAreaName("");
    const std::string regionName = scratch.makeAreaName("region");
    cmed::harness::ProbeArea area{areaName.c_str()};
    cmed::harness::ProbeRegion region{regionName.c_str(), RegionSlots};
    cmed::harness::publishDomain(area.shared(), Slot, DomainName);
    region.createDomain(DomainName);

    // A hold of zero is spent the moment the turn is taken, so the release below ends the run however
    // long the queue is.
    cmed::daemon::DomainManager domains{region.session(), area.shared(),
                                        configWithHold(timing::Millis::zero())};

    request(area.shared(), domains, cmed::protocol::RequestState::LockRequested);

    setQueued(area.shared(), 1);
    release(area.shared(), domains);
    request(area.shared(), domains, cmed::protocol::RequestState::LockRequested);

    ctx.check(cmed::harness::isHeld(area.shared(), Slot), "the queued requester is still granted");
    ctx.checkf(readCount(domains, cmed::observe::DomainEvent::Acquires) == 2, "but on a turn of its own, at %" PRIu64 "", readCount(domains, cmed::observe::DomainEvent::Acquires));
}

void anUnknownDomainIsRefused(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a name the region never had");

    const std::string areaName = scratch.makeAreaName("");
    const std::string regionName = scratch.makeAreaName("region");
    cmed::harness::ProbeArea area{areaName.c_str()};
    cmed::harness::ProbeRegion region{regionName.c_str(), RegionSlots};

    // Published on the cmed side and never created on the cme side, which is what a requester sees
    // after a domain is deleted out from under it.
    cmed::harness::publishDomain(area.shared(), Slot, DomainName);

    cmed::daemon::DomainManager domains{region.session(), area.shared(), configWithHold(CohortHold)};

    request(area.shared(), domains, cmed::protocol::RequestState::LockRequested);

    ctx.check(cmed::harness::readState(area.shared(), Slot) == cmed::protocol::RequestState::Error,
              "the daemon answers Error rather than leaving the requester to time out");
    ctx.checkf(cmed::harness::resolveSlot(area.shared(), Slot).getFailureCode() == -EINVAL,
               "with an errno that says retrying will not help, and it says %d",
               cmed::harness::resolveSlot(area.shared(), Slot).getFailureCode());
    ctx.checkf(readCount(domains, cmed::observe::DomainEvent::Acquires) == 0, "and no turn was taken, at %" PRIu64 "", readCount(domains, cmed::observe::DomainEvent::Acquires));
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"held-domains-probe"};
    return probe::run("cmed held-domain domains",
                      [&scratch](probe::Context& ctx)
                      {
                          aGrantTakesOneTurn(ctx, scratch);
                          aQueuedWaiterKeepsTheTurn(ctx, scratch);
                          theCohortHoldEndsTheRun(ctx, scratch);
                          anUnknownDomainIsRefused(ctx, scratch);
                      });
}
