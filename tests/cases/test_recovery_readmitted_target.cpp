// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_recovery_readmitted_target.cpp -- an authority stalled past its confirm must not seize a slot
// that was freed and re-admitted meanwhile, nor take over what the new occupant holds.
//
// The case frees the slot the way another authority's finish would, while this one is held.

#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "config.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "observe/failpoint.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

constexpr std::uint32_t MaxDomains = 2;  // control + one data
constexpr cme::PeerId MaxPeers = 3;      // authority, doomed peer, keeper; the occupant reuses the doomed slot
constexpr cme::DomainId HeldDomain = 1;  // the only data domain sits right after control
constexpr const char* HeldLane = "lane0";

// The hold is reached after the doomed peer's grace window plus scheduling, never a fixed span.
constexpr std::uint32_t HoldWaitMs = 4000;
constexpr std::uint32_t PeerWaitMs = 4000;
constexpr std::uint32_t TakeoverWaitMs = 1000;
constexpr std::uint32_t ChildWaitMs = 10000;
// Fine, since the takeover after the release is a few writes away.
constexpr std::uint32_t StepMs = 1;
// Long enough for a grant from a live holder, so a refusal means the occupant still holds the lane.
constexpr timing::Millis EntryBudget{100};

// One word per signal in a shared scratch buffer, written by one process and read by another.
enum class Signal : std::uint32_t
{
    DoomedOpen,     // parent: join now
    DoomedOpened,   // doomed: joined
    DoomedDie,      // parent: exit without leaving
    KeeperOpen,     // parent: join and take both domains
    KeeperReady,    // keeper: holds both
    OccupantOpen,   // parent: the doomed slot is free, join into it
    OccupantHolds,  // occupant: holds the lane
    CaseDone,       // parent: the region has been read, leave
    Count,
};

enum class Role : std::uint32_t
{
    Doomed,
    Keeper,
    Occupant,
    Count,
};

[[nodiscard]] cme::PeerId readLaneHolder(const cme::Geometry& region)
{
    return harness::readDomainRecord(region, HeldDomain).getHolder();
}

// What another authority's finish leaves of a peer that held no domain: no participation, None.
void freeMemberSlot(const cme::Geometry& region, cme::PeerId peerId)
{
    static_cast<void>(cme::coherency::rmwIfTrue(region.getMemberSlot(peerId), harness::currentRun().coherency(),
                                                [](auto* member)
                                                {
                                                    if (!member->isValidMagic())
                                                    {
                                                        return false;
                                                    }
                                                    member->storeParticipatingDomains({});
                                                    member->setStatus(Status::None);
                                                    return true;
                                                }));
}

// Leaks its session, so no leave runs: the slot stays Active and stops stamping, holding nothing.
void runDoomed(std::uint32_t* words)
{
    if (!harness::awaitSignal(words, Signal::DoomedOpen, ChildWaitMs))
    {
        return;
    }
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
    static_cast<void>(new cme::Session{harness::openSession()});
    harness::raiseSignal(words, Signal::DoomedOpened);
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
    static_cast<void>(harness::awaitSignal(words, Signal::DoomedDie, ChildWaitMs));
}

// Takes both domains before the death and stays, so the occupant has a live holder to ask.
void runKeeper(std::uint32_t* words)
{
    if (!harness::awaitSignal(words, Signal::KeeperOpen, ChildWaitMs))
    {
        return;
    }
    auto session = harness::openSession();
    session.joinDomain(HeldLane);  // takes the control lock on the way
    static_cast<void>(session.lock(HeldLane));
    harness::raiseSignal(words, Signal::KeeperReady);
    static_cast<void>(harness::awaitSignal(words, Signal::CaseDone, ChildWaitMs));
}

// Joins once the doomed slot is None, so admission hands it that slot, and keeps the lane locked.
void runOccupant(std::uint32_t* words)
{
    if (!harness::awaitSignal(words, Signal::OccupantOpen, ChildWaitMs))
    {
        return;
    }
    auto session = harness::openSession();
    session.joinDomain(HeldLane);
    const auto guard = session.lock(HeldLane);
    harness::raiseSignal(words, Signal::OccupantHolds);
    static_cast<void>(harness::awaitSignal(words, Signal::CaseDone, ChildWaitMs));
}

void runRole(Role role, std::uint32_t* words)
{
    switch (role)
    {
        case Role::Doomed:
            runDoomed(words);
            return;
        case Role::Keeper:
            runKeeper(words);
            return;
        case Role::Occupant:
            runOccupant(words);
            return;
        case Role::Count:
            return;
    }
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so the authority cannot be held");
    }

    harness::formatSession(MaxDomains, MaxPeers);
    const auto region = harness::openBoundRegion();
    const auto signals = ctx.scratch<std::uint32_t>("signals", static_cast<std::uint32_t>(Signal::Count));
    if (!ctx.check(static_cast<bool>(signals), "shared signal buffer mapped"))
    {
        return;
    }
    std::uint32_t* words = signals.data();

    // Forked before the authority opens, so no child copies its poll thread or inherits the hold.
    const std::uint32_t spawned = harness::spawnChildren(static_cast<std::uint32_t>(Role::Count),
                                                         [words](std::uint32_t index)
                                                         {
                                                             runRole(static_cast<Role>(index), words);
                                                         });

    // First in, so the doomed peer's nearest live predecessor and therefore its recovery authority.
    auto authority = harness::openSession();
    authority.createDomain(HeldLane);
    const cme::PeerId authoritySlot = harness::findActiveSlot(region);

    harness::raiseSignal(words, Signal::DoomedOpen);
    const bool doomedUp = harness::awaitSignal(words, Signal::DoomedOpened, PeerWaitMs);
    const cme::PeerId doomed = harness::findActiveSlot(region, authoritySlot);
    harness::raiseSignal(words, Signal::KeeperOpen);
    const bool keeperUp = harness::awaitSignal(words, Signal::KeeperReady, PeerWaitMs);
    const cme::PeerId keeper = harness::findActiveSlot(region, authoritySlot, doomed);
    if (!ctx.check(doomedUp && keeperUp && readLaneHolder(region) == keeper, "the keeper holds the lane before the death"))
    {
        harness::raiseSignal(words, Signal::CaseDone);
        harness::reapChildren(spawned);
        return;
    }

    cme::failpoint::hold(cme::failpoint::Boundary::RecoveryAfterClaim);
    harness::raiseSignal(words, Signal::DoomedDie);
    const bool authorityHeld = cme::failpoint::awaitHeld(timing::Millis{HoldWaitMs});
    const timing::Stopwatch stall;
    const bool claimNamesAuthority = harness::readClaimAuthor(region, doomed) == authoritySlot;

    freeMemberSlot(region, doomed);
    harness::raiseSignal(words, Signal::OccupantOpen);
    const bool occupantHolds = harness::awaitSignal(words, Signal::OccupantHolds, PeerWaitMs) &&
                               harness::hasMemberStatus(region, doomed, Status::Active) &&
                               readLaneHolder(region) == doomed;
    const auto stallMs = stall.elapsed<timing::Millis>();
    cme::failpoint::release();

    const bool takenOver = harness::waitUntil(
        [&region, authoritySlot]
        {
            return readLaneHolder(region) == authoritySlot;
        },
        TakeoverWaitMs, StepMs);
    const bool enteredTwice = harness::canLock(authority, HeldLane, EntryBudget);
    std::printf("  authority %u, doomed and occupant %u, keeper %u; authority held %" PRId64 " ms past its confirm\n",
                authoritySlot, doomed, keeper, static_cast<std::int64_t>(stallMs.count()));

    harness::raiseSignal(words, Signal::CaseDone);
    harness::reapChildren(spawned);

    ctx.check(authorityHeld, "the authority confirmed its claim on the doomed peer and stopped before seizing");
    ctx.check(claimNamesAuthority, "the claim on the doomed slot named the authority");
    ctx.check(occupantHolds, "a new peer took the freed slot and locked the lane under that id");
    ctx.check(stallMs < cme::config::recovery::TickStall, "the authority's stall stayed inside TickStall");
    ctx.check(!takenOver, "the lane was not taken over while its occupant held it");
    ctx.check(!enteredTwice, "the authority could not enter the lane its occupant held");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
