// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_admission_late_stake.cpp -- a joiner whose stake lands late must not take the lease.
//
// A joiner that read the lease free and was then descheduled stores its nonce after a faster
// joiner's re-read already confirmed. The AdmissionBeforeStake hold puts the joiner in that gap, and
// the faster joiner is played by hand: its nonce goes in while the joiner is held, its slot and bound
// after. Two more holds stretch the late joiner's withdrawal, or stop it before the withdrawal.

#include <atomic>
#include <cstdint>
#include <thread>

#include "admission/claim.hpp"
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

using Boundary = cme::failpoint::Boundary;
using Status = cme::Geometry::Member_t::Status;

constexpr std::int64_t Pending = -1;
constexpr std::int64_t Threw = -2;

constexpr std::uint32_t Domains = 1;
// The joiner reserves the lowest free slot, so a joiner that waited for the holder lands one above it.
constexpr cme::PeerId HolderSlot = 0;
// The confirmed holder's nonce. Any nonzero value reads as held.
constexpr std::uint64_t HolderNonce = 0xC0FFEE00D15EA5EDULL;
// Reaching a hold takes one thread getting scheduled, not any work.
constexpr timing::Millis HoldWait{2000};
// The late nonce lands one store after the joiner is let go.
constexpr std::uint32_t LandWaitMs = 2000;
constexpr std::uint32_t LandPollMs = 1;

// claimPeerSlot on a thread of its own, which stores the slot it got, or Threw, in @claimed.
[[nodiscard]] std::thread startJoiner(const cme::Geometry& region, std::atomic<std::int64_t>& claimed)
{
    return std::thread(
        [&region, &claimed]
        {
            try
            {
                claimed.store(cme::admission::claimPeerSlot(region), std::memory_order_release);
            }
            catch (...)
            {
                claimed.store(Threw, std::memory_order_release);
            }
        });
}

// With a joiner held at AdmissionBeforeStake, lands the holder's nonce and lets the stake go in late.
// Returns the late nonce once the line carries it, or 0 when the joiner never got that far.
[[nodiscard]] std::uint64_t landLate(const cme::Geometry& region, cme::CoherencyMode coherency)
{
    auto* control = region.getAdmissionControl();
    const bool held = cme::failpoint::awaitHeld(HoldWait);
    if (held)
    {
        auto line = cme::coherency::get(control, coherency);
        line.nonce = HolderNonce;
        cme::coherency::set(control, line, coherency);
        // Past the stake window, so the joiner's stake is late however the clock is read.
        std::this_thread::sleep_for(cme::config::stake::Window * 2);
    }
    cme::failpoint::release();

    std::uint64_t lateNonce = 0;
    const bool landed = harness::waitUntil(
        [&]
        {
            lateNonce = cme::coherency::get(control, coherency).nonce;
            return lateNonce != HolderNonce;
        },
        LandWaitMs, LandPollMs);
    return held && landed ? lateNonce : 0;
}

// The writes the holder still owes past its confirmation. No release follows: the holder's checked
// release finds the late nonce in the line and leaves it.
[[nodiscard]] bool finishHolderClaim(const cme::Geometry& region, cme::CoherencyMode coherency)
{
    const bool reserved = cme::coherency::rmwIfTrue(
        region.getMemberSlot(HolderSlot), coherency,
        [](auto* member)
        {
            if (!member->isValidMagic() || !member->hasStatus(Status::None))
            {
                return false;
            }
            member->setStatus(Status::Active);
            return true;
        });
    auto* control = region.getAdmissionControl();
    control->peerScanBound = HolderSlot + 1;
    cme::coherency::wmb(control, sizeof(*control), coherency);
    return reserved;
}

// The holder's slot and bound land inside the late stake's hold. The joiner, having withdrawn its
// nonce, takes the slot above on a fresh stake and never waits out a steal.
void checkWithdrawAfterHolder(harness::TestContext& ctx)
{
    constexpr std::uint32_t Peers = 2;
    constexpr cme::PeerId JoinerSlot = HolderSlot + 1;
    const cme::CoherencyMode coherency = ctx.coherency();
    const cme::Geometry region = harness::createRegion(Domains, Peers);
    auto* control = region.getAdmissionControl();

    std::atomic<std::int64_t> claimed{Pending};
    // Armed before the joiner starts, so the reach cannot beat the arm.
    cme::failpoint::hold(Boundary::AdmissionBeforeStake);
    std::thread joiner = startJoiner(region, claimed);
    const std::uint64_t lateNonce = landLate(region, coherency);
    const timing::Stopwatch sinceLate;

    // A late stake that won would return one settle after it landed. Three leave room for scheduling.
    constexpr auto WinWindow = cme::config::stake::Settle * 3;
    std::this_thread::sleep_for(WinWindow);
    const std::int64_t claimedEarly = claimed.load(std::memory_order_acquire);

    // Half the writer stall on, so the holder's last write lands inside the late stake's hold.
    constexpr auto HolderTail = cme::config::lease::WriterStall / 2;
    std::this_thread::sleep_for(HolderTail);
    const bool holderReserved = finishHolderClaim(region, coherency);
    const std::int64_t claimedAfterHolder = claimed.load(std::memory_order_acquire);

    joiner.join();
    const double waitedMs = sinceLate.elapsed<timing::MillisF>().count();
    const std::int64_t claimedFinal = claimed.load(std::memory_order_acquire);
    const auto finalLine = cme::coherency::get(control, coherency);

    ctx.check(lateNonce != 0, "the joiner reached its stake after reading the lease free, and staked late");
    ctx.check(claimedEarly == Pending, "a late stake did not take the lease the holder still held");
    ctx.check(holderReserved, "the holder reserved its slot after the late stake landed");
    ctx.check(claimedAfterHolder == Pending, "the joiner stayed out until the holder's last write");
    ctx.check(claimedFinal == static_cast<std::int64_t>(JoinerSlot),
              "the joiner got in on a fresh stake after withdrawing, on the slot above the holder's");
    ctx.checkf(waitedMs >= timing::MillisF(cme::config::lease::LateStakeHold).count() &&
                   waitedMs < timing::MillisF(cme::config::lease::Timeout).count(),
               "the joiner held its late stake and withdrew it rather than waiting out a steal (%.0f ms)",
               waitedMs);
    ctx.check(finalLine.nonce == 0, "the joiner released the lease on the way out");
    ctx.check(finalLine.peerScanBound == JoinerSlot + 1,
              "the scan bound covers the holder's slot and the joiner's");
}

// The late joiner stalls between finding its own nonce and clearing it, inside the writer stall the
// steal assumes. A second joiner watching that nonce gets in on the clear, not by a steal.
void checkDelayedWithdraw(harness::TestContext& ctx)
{
    constexpr std::uint32_t Peers = 3;
    const cme::CoherencyMode coherency = ctx.coherency();
    const cme::Geometry region = harness::createRegion(Domains, Peers);
    auto* control = region.getAdmissionControl();

    std::atomic<std::int64_t> lateClaimed{Pending};
    std::atomic<std::int64_t> watcherClaimed{Pending};
    cme::failpoint::hold(Boundary::AdmissionBeforeStake);
    std::thread lateJoiner = startJoiner(region, lateClaimed);
    const std::uint64_t lateNonce = landLate(region, coherency);
    // Armed while the late joiner sits in its hold, which outlasts the rest of this setup.
    cme::failpoint::hold(Boundary::AdmissionReleaseAfterCheck);
    const bool holderReserved = finishHolderClaim(region, coherency);

    const timing::Stopwatch sinceWatch;
    std::thread watcher = startJoiner(region, watcherClaimed);
    const bool stalled = cme::failpoint::awaitHeld(HoldWait);
    constexpr auto WithdrawStall = cme::config::lease::WriterStall / 2;
    std::this_thread::sleep_for(WithdrawStall);
    const bool watcherOut = watcherClaimed.load(std::memory_order_acquire) == Pending;
    const bool lateStood = cme::coherency::get(control, coherency).nonce == lateNonce;
    cme::failpoint::release();

    watcher.join();
    const double watcherMs = sinceWatch.elapsed<timing::MillisF>().count();
    lateJoiner.join();
    const std::int64_t late = lateClaimed.load(std::memory_order_acquire);
    const std::int64_t watched = watcherClaimed.load(std::memory_order_acquire);
    const auto finalLine = cme::coherency::get(control, coherency);

    const auto isAboveHolder = [](std::int64_t slot)
    {
        return slot > static_cast<std::int64_t>(HolderSlot) && slot < static_cast<std::int64_t>(Peers);
    };
    ctx.check(lateNonce != 0, "delayed withdraw: the first joiner staked late");
    ctx.check(holderReserved, "delayed withdraw: the holder reserved its slot inside the hold");
    ctx.check(stalled, "delayed withdraw: the late joiner stopped between its check and its clear");
    ctx.check(watcherOut && lateStood, "delayed withdraw: the watcher stayed out while the late nonce stood");
    ctx.check(isAboveHolder(late) && isAboveHolder(watched) && late != watched,
              "delayed withdraw: both joiners got in, each on its own slot above the holder's");
    ctx.checkf(watcherMs < timing::MillisF(cme::config::lease::Timeout).count(),
               "delayed withdraw: the watcher got in on the clear, not by a steal (%.0f ms)", watcherMs);
    ctx.check(finalLine.nonce == 0 && finalLine.peerScanBound == Peers,
              "delayed withdraw: the lease is free and the scan bound covers every slot");
}

// The late joiner stops after its hold and before its withdrawal, which is what a joiner that died
// there leaves. A second joiner gets in by stealing that nonce, and the stopped one then clears nothing.
void checkStopBeforeWithdraw(harness::TestContext& ctx)
{
    constexpr std::uint32_t Peers = 3;
    constexpr cme::PeerId WatcherSlot = HolderSlot + 1;
    constexpr cme::PeerId ResumedSlot = WatcherSlot + 1;
    const cme::CoherencyMode coherency = ctx.coherency();
    const cme::Geometry region = harness::createRegion(Domains, Peers);
    auto* control = region.getAdmissionControl();

    std::atomic<std::int64_t> lateClaimed{Pending};
    std::atomic<std::int64_t> watcherClaimed{Pending};
    cme::failpoint::hold(Boundary::AdmissionBeforeStake);
    std::thread lateJoiner = startJoiner(region, lateClaimed);
    const std::uint64_t lateNonce = landLate(region, coherency);
    cme::failpoint::hold(Boundary::AdmissionBeforeWithdraw);
    const bool holderReserved = finishHolderClaim(region, coherency);
    const bool stopped = cme::failpoint::awaitHeld(HoldWait);

    const timing::Stopwatch sinceWatch;
    std::thread watcher = startJoiner(region, watcherClaimed);
    watcher.join();
    const double watcherMs = sinceWatch.elapsed<timing::MillisF>().count();
    const bool lateGone = cme::coherency::get(control, coherency).nonce != lateNonce;
    cme::failpoint::release();

    lateJoiner.join();
    const auto finalLine = cme::coherency::get(control, coherency);

    ctx.check(lateNonce != 0, "stopped joiner: the first joiner staked late");
    ctx.check(holderReserved, "stopped joiner: the holder reserved its slot inside the hold");
    ctx.check(stopped, "stopped joiner: the late joiner stopped before withdrawing its nonce");
    ctx.check(watcherClaimed.load(std::memory_order_acquire) == static_cast<std::int64_t>(WatcherSlot),
              "stopped joiner: the watcher got in, on the slot above the holder's");
    ctx.checkf(watcherMs >= timing::MillisF(cme::config::lease::Timeout).count(),
               "stopped joiner: the watcher got in by stealing the nonce left behind (%.0f ms)", watcherMs);
    ctx.check(lateGone, "stopped joiner: the steal and release moved the line off the late nonce");
    ctx.check(lateClaimed.load(std::memory_order_acquire) == static_cast<std::int64_t>(ResumedSlot),
              "stopped joiner: once resumed it cleared nothing and took the next slot");
    ctx.check(finalLine.nonce == 0 && finalLine.peerScanBound == Peers,
              "stopped joiner: the lease is free and the scan bound covers every slot");
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip(
            "built without CME_FAILPOINT, so the stake cannot be held after the read");
    }

    checkWithdrawAfterHolder(ctx);
    checkDelayedWithdraw(ctx);
    checkStopBeforeWithdraw(ctx);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
