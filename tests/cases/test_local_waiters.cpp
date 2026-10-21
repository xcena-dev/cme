// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_local_waiters.cpp -- one thread of a peer giving up on a domain keeps another thread's wait.
//
// A peer's threads share one demand bit under REQUEST and RequestAgg, and one leaf of the tree under
// Peterson. A short wait times out while a long wait on the same domain is pending, and the holder
// releases only after that: the short wait must not get in, and the long wait must still be granted.

#include <cstdint>
#include <optional>
#include <thread>

#include "cme/shared.hpp"
#include "common/ordered.hpp"
#include "common/timing.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr std::uint32_t Domains = 2;
constexpr std::uint32_t Peers = 2;
constexpr const char* LaneName = "lane";

// The long wait outlasts everything below many times over; the short one ends well before the release.
constexpr timing::Millis LongBudget{3000};
constexpr timing::Millis ShortBudget{2};
// Lets the long wait raise its request before the short one starts.
constexpr timing::Millis RaiseGap{20};
// Past the short wait's timeout and its withdraw settle, so the holder releases into what it left.
constexpr timing::Millis ReleaseGap{100};

}  // namespace

void runBody(harness::TestContext& ctx)
{
    harness::formatSession(Domains, Peers);
    auto holder = harness::openSession();
    holder.createDomain(LaneName);
    auto held = holder.tryLock(LaneName, LongBudget);
    ctx.check(held.has_value(), "the holder takes the domain first");

    auto waiter = harness::openSession();
    waiter.joinDomain(LaneName);

    ordered::Published<bool> longGranted{false};
    std::thread longWait(
        [&]
        {
            const auto granted = waiter.tryLock(LaneName, LongBudget);
            longGranted.store(granted.has_value());
        });

    std::this_thread::sleep_for(RaiseGap);
    const bool shortGaveUp = !waiter.tryLock(LaneName, ShortBudget).has_value();
    std::this_thread::sleep_for(ReleaseGap);
    held.reset();

    const timing::Stopwatch sinceRelease;
    longWait.join();
    const double grantMs = sinceRelease.elapsed<timing::MillisF>().count();

    ctx.check(shortGaveUp, "the short wait gave up while the holder kept the domain");
    ctx.check(longGranted.load(),
              "the long wait of the same peer was granted once the holder released");
    ctx.checkf(grantMs < timing::MillisF(LongBudget).count() / 2,
               "the long wait was granted on the release, not at the end of its budget (%.0f ms)", grantMs);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
