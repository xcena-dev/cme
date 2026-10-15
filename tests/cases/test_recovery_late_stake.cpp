// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_recovery_late_stake.cpp -- a recovery claim staked late must not leave two holders on one epoch.
//
// The late authority is held between reading the claim free and staking it. The early authority, a
// child of its own, confirms meanwhile and stalls inside its takeover, between the shadow and the
// truth write, for less than config::recovery::TickStall. The late stake deposes it, and must not act on the
// dead peer's records before that stalled write lands.

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "config.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "observe/failpoint.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;
using RecordState = cme::Geometry::DomainRecord_t::State;

constexpr std::uint32_t MaxDomains = 2;  // control + one data
constexpr cme::PeerId MaxPeers = 3;      // the doomed peer and two authorities
constexpr const char* HeldLane = "lane0";

// Each hold is reached after the doomed peer's grace window plus scheduling, never a fixed span.
constexpr std::uint32_t HoldWaitMs = 4000;
// The early authority's stall after the late stake, inside the bound the settle is sized for.
constexpr std::uint32_t EarlyStallMs = static_cast<std::uint32_t>(cme::config::recovery::TickStall.count() / 2);
// A write after a release, and a whole recovery after the settle: generous, since both are polled.
constexpr std::uint32_t WriteWaitMs = 1000;
constexpr std::uint32_t RecoveryWaitMs = 5000;
constexpr std::uint32_t ChildWaitMs = 10000;
// Fine, since a signal late by the harness's default step would let the next write pass it.
constexpr std::uint32_t StepMs = 1;

// One word per signal in a shared scratch buffer, written by one process and read by the other.
enum class Signal : std::uint32_t
{
    EarlyStart,  // parent: the late authority is held, join now
    EarlyHeld,   // child: Reached or Missed
    EarlyGo,     // parent: let the held takeover write its truth
    CaseDone,    // parent: the region has been read, leave
    Count,
};
constexpr std::uint32_t Reached = 1;
constexpr std::uint32_t Missed = 2;

// Whether some live domain's truth names @peerId.
[[nodiscard]] bool truthNames(cme::Geometry& region, cme::PeerId peerId)
{
    for (cme::DomainId domainId = 0; domainId < MaxDomains; ++domainId)
    {
        if (harness::readDomainRecord(region, domainId).getHolder() == peerId)
        {
            return true;
        }
    }
    return false;
}

// A live domain whose truth and some group's shadow carry one epoch under two holders. A Free record
// is skipped: the orphan sweep frees it with the epoch it had and no holder.
[[nodiscard]] bool findDoubledEpoch(cme::Geometry& region)
{
    bool doubled = false;
    for (cme::DomainId domainId = 0; domainId < MaxDomains; ++domainId)
    {
        const auto truth = harness::readDomainRecord(region, domainId);
        if (!truth.hasState(RecordState::Active) || cme::isNoPeer(truth.getHolder()))
        {
            continue;
        }
        const auto truthEpoch = static_cast<std::uint64_t>(truth.epoch);
        for (cme::PeerId peerId = 0; peerId < MaxPeers; ++peerId)
        {
            const auto shadow = harness::readDomainRecordShadow(region, domainId, peerId);
            if (static_cast<std::uint64_t>(shadow.epoch) == truthEpoch && shadow.getHolder() != truth.getHolder())
            {
                std::printf("  domain %u epoch %" PRIu64 ": truth names %u, the shadow peer %u polls names %u\n",
                            domainId, truthEpoch, truth.getHolder(), peerId, shadow.getHolder());
                doubled = true;
            }
        }
    }
    return doubled;
}

// The early authority. It joins only when told, so the late one picked its target before it existed.
void runEarlyAuthority(std::uint32_t* words)
{
    if (!harness::awaitSignal(words, Signal::EarlyStart, ChildWaitMs))
    {
        return;
    }
    cme::failpoint::hold(cme::failpoint::Boundary::TransferBeforeTruth);
    std::thread watcher(
        [words]
        {
            const bool held = cme::failpoint::awaitHeld(timing::Millis{HoldWaitMs});
            harness::raiseSignal(words, Signal::EarlyHeld, held ? Reached : Missed);
            static_cast<void>(harness::awaitSignal(words, Signal::EarlyGo, ChildWaitMs));
            cme::failpoint::release();
        });
    // Departs only after CaseDone: its leave rewrites the records the parent checks.
    const auto session = harness::openSession();
    watcher.join();
    static_cast<void>(harness::awaitSignal(words, Signal::CaseDone, ChildWaitMs));
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so neither authority can be held");
    }

    harness::formatSession(MaxDomains, MaxPeers);
    {
        auto seeder = harness::openSession();
        seeder.createDomain(HeldLane);
    }
    auto region = harness::openBoundRegion();

    // The doomed peer inherits both domains from the departed seeder and dies holding them.
    harness::reapChildren(harness::spawnChildren(
        1,
        [](std::uint32_t)
        {
            auto* session = new cme::Session{harness::openSession()};
            session->joinDomain(HeldLane);
            static_cast<void>(new cme::Guard{session->lock(HeldLane)});
        }));
    const cme::PeerId doomed = harness::findActiveSlot(region);
    if (!ctx.check(!cme::isNoPeer(doomed), "the doomed peer left an Active slot behind"))
    {
        return;
    }

    const auto signals = ctx.scratch<std::uint32_t>("signals", static_cast<std::uint32_t>(Signal::Count));
    if (!ctx.check(static_cast<bool>(signals), "shared signal buffer mapped"))
    {
        return;
    }
    std::uint32_t* words = signals.data();

    // Forked before the parent opens a session, so the child copies no poll thread it cannot join.
    const std::uint32_t spawned = harness::spawnChildren(1,
                                                         [words](std::uint32_t)
                                                         {
                                                             runEarlyAuthority(words);
                                                         });

    cme::failpoint::hold(cme::failpoint::Boundary::RecoveryBeforeStake);
    auto lateSession = harness::openSession();
    const bool lateHeld = cme::failpoint::awaitHeld(timing::Millis{HoldWaitMs});
    const cme::PeerId late = harness::findActiveSlot(region, doomed);

    harness::raiseSignal(words, Signal::EarlyStart);
    const bool earlyHeld = harness::awaitSignal(words, Signal::EarlyHeld, HoldWaitMs) &&
                           harness::readSignal(words, Signal::EarlyHeld) == Reached;
    const cme::PeerId early = harness::findActiveSlot(region, doomed, late);

    cme::failpoint::release();
    harness::sleepMs(EarlyStallMs);
    const bool lateActedEarly = truthNames(region, late);

    harness::raiseSignal(words, Signal::EarlyGo);
    const bool earlyWrote = harness::waitUntil(
        [&region, early]
        {
            return truthNames(region, early);
        },
        WriteWaitMs, StepMs);
    const bool recovered = harness::waitUntil(
        [&region, doomed]
        {
            return harness::hasMemberStatus(region, doomed, Status::None);
        },
        RecoveryWaitMs, StepMs);
    std::printf("  doomed %u, late authority %u, early authority %u\n", doomed, late, early);
    const bool doubled = findDoubledEpoch(region);

    harness::raiseSignal(words, Signal::CaseDone);
    harness::reapChildren(spawned);

    ctx.check(lateHeld, "the late authority reached its stake after reading the claim free");
    ctx.check(earlyHeld, "the early authority confirmed and stopped inside its takeover");
    ctx.check(!lateActedEarly, "the late authority took nothing over while the early one stalled");
    ctx.check(earlyWrote, "the early authority's stalled truth write landed");
    ctx.check(recovered, "the doomed peer's slot reached None");
    ctx.check(!doubled, "no live domain carries two holders at one epoch");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
