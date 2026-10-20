// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_delete_late_joiner.cpp -- a delete must refuse while a peer that joined after the deleter's last
// scan refresh takes part in the domain.
//
// The deleter's poll thread is held before its next scan refresh, so its cached peer bound predates the
// late joiner. The region's bound line already covers that joiner, and the delete must still see it.

#include <cstdint>
#include <cstdio>
#include <exception>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/timing.hpp"
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

constexpr std::uint32_t MaxDomains = 3;  // control, the shared lane, and the side lane
constexpr cme::PeerId MaxPeers = 3;      // the deleter, the control taker, and the late joiner
constexpr cme::PeerId LateJoinerSlot = 2;
constexpr const char* SharedLane = "lane0";
constexpr const char* SideLane = "lane1";

// The hold lands on the next scan refresh, one ScanRefreshInterval away at most.
constexpr std::uint32_t HoldWaitMs = 2000;
constexpr std::uint32_t PeerWaitMs = 4000;
constexpr std::uint32_t ChildWaitMs = 10000;

// One word per signal in a shared scratch buffer, written by one process and read by another.
enum class Signal : std::uint32_t
{
    ChildOpen,     // parent: the shared lane exists
    ControlMoved,  // child: the control taker holds the control record
    ScanHeld,      // parent: its poll thread is held before a scan refresh
    LateJoined,    // child: 1 when the late joiner is in the shared lane, 2 when its join failed
    CaseDone,      // parent: the delete has been tried, exit
    Count,
};

constexpr std::uint32_t JoinOk = 1;
constexpr std::uint32_t JoinFailed = 2;

enum class DeleteOutcome : std::uint8_t
{
    NotTried,
    Refused,
    Deleted,
    Failed,
};

// The control taker moves the control record off the parent first, since a held poll thread grants
// nothing and the late joiner's join needs that lock.
void runChild(std::uint32_t* words)
{
    if (!harness::awaitSignal(words, Signal::ChildOpen, ChildWaitMs))
    {
        return;
    }
    auto controlTaker = harness::openSession();
    controlTaker.createDomain(SideLane);
    harness::raiseSignal(words, Signal::ControlMoved);

    if (!harness::awaitSignal(words, Signal::ScanHeld, ChildWaitMs))
    {
        return;
    }
    auto lateJoiner = harness::openSession();
    try
    {
        lateJoiner.joinDomain(SharedLane);
        harness::raiseSignal(words, Signal::LateJoined, JoinOk);
    }
    catch (const std::exception& failure)
    {
        harness::log("late join failed: %s", failure.what());
        harness::raiseSignal(words, Signal::LateJoined, JoinFailed);
    }
    static_cast<void>(harness::awaitSignal(words, Signal::CaseDone, ChildWaitMs));
}

[[nodiscard]] DeleteOutcome tryDelete(cme::Session& deleter)
{
    try
    {
        deleter.deleteDomain(SharedLane);
        return DeleteOutcome::Deleted;
    }
    catch (const cme::NotParticipatingError&)
    {
        return DeleteOutcome::Refused;
    }
    catch (const std::exception& failure)
    {
        harness::log("delete failed: %s", failure.what());
        return DeleteOutcome::Failed;
    }
}

[[nodiscard]] const char* readOutcomeName(DeleteOutcome outcome)
{
    switch (outcome)
    {
        case DeleteOutcome::NotTried:
            return "not tried";
        case DeleteOutcome::Refused:
            return "refused";
        case DeleteOutcome::Deleted:
            return "deleted";
        case DeleteOutcome::Failed:
            return "failed";
    }
    return "unknown";
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so the scan refresh cannot be held");
    }
    if (ctx.strategy() == cme::Strategy::Order)
    {
        harness::TestContext::skip("the order token passes to every participant unasked, so it stops at the held deleter");
    }

    harness::formatSession(MaxDomains, MaxPeers);
    const auto signals = ctx.scratch<std::uint32_t>("signals", static_cast<std::uint32_t>(Signal::Count));
    if (!ctx.check(static_cast<bool>(signals), "shared signal buffer mapped"))
    {
        return;
    }
    std::uint32_t* words = signals.data();

    // Forked before the parent opens, so the child copies no poll thread and inherits no hold.
    const std::uint32_t spawned = harness::spawnChildren(1,
                                                         [words](std::uint32_t)
                                                         {
                                                             runChild(words);
                                                         });

    auto deleter = harness::openSession();
    const cme::DomainHandle_t sharedLane = deleter.createDomain(SharedLane);
    harness::raiseSignal(words, Signal::ChildOpen);
    const bool controlMoved = harness::awaitSignal(words, Signal::ControlMoved, PeerWaitMs);

    cme::failpoint::hold(cme::failpoint::Boundary::ScanBeforeRefresh);
    const bool scanHeld = controlMoved && cme::failpoint::awaitHeld(timing::Millis{HoldWaitMs});
    if (scanHeld)
    {
        harness::raiseSignal(words, Signal::ScanHeld);
    }
    const bool lateJoined = scanHeld && harness::awaitSignal(words, Signal::LateJoined, PeerWaitMs) &&
                            harness::readSignal(words, Signal::LateJoined) == JoinOk;

    const cme::Geometry region = harness::openBoundRegion();
    const std::uint32_t regionBound =
        cme::coherency::get(region.getPeerBound(), harness::currentRun().coherency()).peerScanBound;
    const bool joinerParticipates =
        harness::readMemberSlot(region, LateJoinerSlot).loadParticipatingDomains().has(sharedLane.id);

    const DeleteOutcome outcome = lateJoined ? tryDelete(deleter) : DeleteOutcome::NotTried;
    cme::failpoint::release();
    std::printf("  region bound %u, late joiner in the lane: %s, delete: %s\n", regionBound,
                joinerParticipates ? "yes" : "no", readOutcomeName(outcome));

    harness::raiseSignal(words, Signal::CaseDone);
    harness::reapChildren(spawned);

    ctx.check(controlMoved, "the control taker took the control record off the deleter");
    ctx.check(scanHeld, "the deleter's poll thread stopped before a scan refresh");
    ctx.check(lateJoined, "the late joiner joined the shared lane while that refresh was held");
    ctx.check(regionBound > LateJoinerSlot && joinerParticipates,
              "the region's bound line covers the late joiner, and its slot names the lane");
    ctx.check(outcome == DeleteOutcome::Refused, "the delete refused while the late joiner takes part");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
