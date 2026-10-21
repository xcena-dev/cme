// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_peterson_cancelled_waiter.cpp -- a thread whose tryLock gave up must not keep the tournament
// from being released.
//
// The last guard's unlock reads the domain's pin count under the climb lock, and keeps the root when
// another pin is there. Two failpoints stop that unlock before the read and a tryLock refused the
// climb lock before it returns, and the case releases them in each order.

#include <cstdint>
#include <cstdio>
#include <thread>

#include "common/ordered.hpp"
#include "common/timing.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "observe/failpoint.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

using Boundary = cme::failpoint::Boundary;

constexpr std::uint32_t FormatDomains = 2;  // control + the one data domain
constexpr std::uint32_t FormatPeers = 2;

constexpr cme::PeerId OwnerId = 0;
constexpr cme::PeerId OtherId = 1;

// Reaching a hold takes one thread getting scheduled, not any work.
constexpr timing::Millis HoldWait{2000};
// Zero is one probe of the climb lock, which the unlock in progress holds.
constexpr timing::Nanos NoBudget{0};
// A free tournament is won in microseconds, so only an interest nobody will retract outlasts this.
constexpr timing::Millis AcquireWindow{500};
constexpr std::uint32_t ReturnWaitMs = 2000;
constexpr std::uint32_t ReturnPollMs = 1;

enum class Order : std::uint8_t
{
    WaiterFirst,  // the refused tryLock returns before the unlock reads the count
    UnlockFirst,  // the unlock reads the count while the refused tryLock has not returned
};

[[nodiscard]] bool returnsWithin(const ordered::Published<bool>& done)
{
    return harness::waitUntil(
        [&done]
        {
            return done.load();
        },
        ReturnWaitMs, ReturnPollMs);
}

// Releases @first and waits for its thread to return, then does the same for @second.
[[nodiscard]] bool releaseInTurn(Boundary first, const ordered::Published<bool>& firstDone, Boundary second,
                                 const ordered::Published<bool>& secondDone)
{
    cme::failpoint::release(first);
    const auto firstReturned = returnsWithin(firstDone);
    cme::failpoint::release(second);
    const auto secondReturned = returnsWithin(secondDone);
    return firstReturned && secondReturned;
}

void checkOrdering(harness::TestContext& ctx, Order order, const char* label)
{
    auto region = harness::createRegion(FormatDomains, FormatPeers);
    auto owner = harness::makePeer(region, OwnerId);
    auto other = harness::makePeer(region, OtherId);
    const auto domain = owner.createDomain("lane").id;
    owner.joinDomain(domain);
    other.joinDomain(domain);

    // Armed after the setup, whose control-domain unlocks would stop at the same boundary.
    auto guard = owner.lock(domain);
    cme::failpoint::hold(Boundary::PetersonUnlockBeforePins);
    cme::failpoint::hold(Boundary::PetersonLockRefused);

    ordered::Published<bool> unlocked{false};
    std::thread unlocker(
        [&]
        {
            guard.release();
            unlocked.store(true);
        });
    const auto unlockHeld = cme::failpoint::awaitHeld(Boundary::PetersonUnlockBeforePins, HoldWait);

    ordered::Published<bool> waiterDone{false};
    ordered::Published<bool> waiterGotLock{true};
    std::thread waiter(
        [&]
        {
            waiterGotLock.store(harness::canLock(owner, domain, NoBudget));
            waiterDone.store(true);
        });
    const auto waiterHeld = cme::failpoint::awaitHeld(Boundary::PetersonLockRefused, HoldWait);

    const auto bothReturned = (order == Order::WaiterFirst)
                                  ? releaseInTurn(Boundary::PetersonLockRefused, waiterDone,
                                                  Boundary::PetersonUnlockBeforePins, unlocked)
                                  : releaseInTurn(Boundary::PetersonUnlockBeforePins, unlocked,
                                                  Boundary::PetersonLockRefused, waiterDone);
    // Both again, so a boundary the checks below found unreached cannot keep a thread from joining.
    cme::failpoint::release(Boundary::PetersonUnlockBeforePins);
    cme::failpoint::release(Boundary::PetersonLockRefused);
    unlocker.join();
    waiter.join();

    const auto acquired = harness::canLock(other, domain, AcquireWindow);

    // Whatever stayed behind goes with one more lock and unlock, so the next section starts clean.
    if (!acquired)
    {
        const auto cleaned = harness::canLock(owner, domain, AcquireWindow);
        const auto reacquired = cleaned && harness::canLock(other, domain, AcquireWindow);
        std::printf("  %s: the other peer was refused; after the owner cycled the domain it %s\n", label,
                    reacquired ? "acquired" : "was refused again");
    }

    ctx.checkf(unlockHeld, "%s: the unlock stopped under the climb lock before reading the pin count", label);
    ctx.checkf(waiterHeld, "%s: the second thread was refused the climb lock and stopped before returning",
               label);
    ctx.checkf(bothReturned, "%s: both threads ran to the end once released", label);
    ctx.checkf(!waiterGotLock.load(), "%s: the zero-budget tryLock returned no guard",
               label);
    ctx.checkf(acquired, "%s: the other peer acquired the domain once the owner held no guard", label);
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so the unlock cannot be stopped under its lock");
    }

    checkOrdering(ctx, Order::WaiterFirst, "control ordering");
    checkOrdering(ctx, Order::UnlockFirst, "racing ordering");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
