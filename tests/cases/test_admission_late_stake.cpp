// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_admission_late_stake.cpp -- a joiner whose stake lands late must not take the lease.
//
// A joiner that read the lease free and was then descheduled stores its nonce after a faster
// joiner's re-read already confirmed. The AdmissionBeforeStake hold puts the joiner in that gap,
// and the faster joiner is played by hand: its nonce goes into the line while the joiner is held.

#include <atomic>
#include <cstdint>
#include <thread>

#include "admission/claim.hpp"
#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "config.hpp"
#include "core/layout/geometry.hpp"
#include "helper.hpp"
#include "observe/failpoint.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip(
            "built without CME_FAILPOINT, so the stake cannot be held after the read");
    }

    constexpr std::uint32_t Domains = 1;
    constexpr std::uint32_t Peers = 2;
    const cme::CoherencyMode coherency = ctx.coherency();
    cme::Geometry region = harness::createRegion(Domains, Peers);
    auto* control = region.getAdmissionControl();

    // Armed before the joiner starts, so the reach cannot beat the arm.
    cme::failpoint::hold(cme::failpoint::Boundary::AdmissionBeforeStake);

    constexpr std::int64_t Pending = -1;
    constexpr std::int64_t Threw = -2;
    std::atomic<std::int64_t> claimed{Pending};
    std::thread joiner(
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

    // Reaching the hold takes one thread getting scheduled, not any work.
    constexpr timing::Millis HoldWait{2000};
    const bool held = cme::failpoint::awaitHeld(HoldWait);
    if (held)
    {
        // The confirmed holder's nonce. Any nonzero value reads as held.
        constexpr std::uint64_t HolderNonce = 0xC0FFEE00D15EA5EDULL;
        auto line = cme::coherency::get(control, coherency);
        line.nonce = HolderNonce;
        cme::coherency::set(control, line, coherency);
        // Past the stake window, so the joiner's stake is late however the clock is read.
        std::this_thread::sleep_for(cme::StakeWindow * 2);
    }
    const timing::Stopwatch sinceRelease;
    cme::failpoint::release();

    // A late stake that won would return one settle after the release. Three leave room for scheduling.
    constexpr auto WinWindow = cme::ClaimSettle * 3;
    std::this_thread::sleep_for(WinWindow);
    const std::int64_t claimedEarly = claimed.load(std::memory_order_acquire);
    joiner.join();
    const double waitedMs = sinceRelease.elapsed<timing::MillisF>().count();
    const std::int64_t claimedFinal = claimed.load(std::memory_order_acquire);

    ctx.check(held, "the joiner reached its stake after reading the lease free");
    ctx.check(claimedEarly == Pending, "a late stake did not take the lease the holder still held");
    ctx.check(claimedFinal >= 0, "the joiner got in once the line looked abandoned");
    ctx.checkf(waitedMs >= timing::MillisF(cme::LeaseTimeout).count(),
               "the joiner waited out the stall before stealing (%.0f ms)", waitedMs);
    ctx.check(cme::coherency::get(control, coherency).nonce == 0,
              "the joiner released the lease on the way out");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
