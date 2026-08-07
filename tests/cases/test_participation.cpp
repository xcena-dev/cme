// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_participation.cpp -- Phase 2 opt-in participation API.
//
// A freshly opened session participates everywhere by default, so the guard is driven by
// explicitly leaving a domain and re-joining it. Covers:
//   - default participation: lock works without an explicit join
//   - leaveDomain -> lock / tryLock on that domain now refused
//   - joinDomain re-enables lock (re-baseline + re-advertise)
//   - sole-participant leave is refused (would orphan the domain)
//   - joinDomain idempotent
//   - unknown domain name -> UnknownDomainError
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.

#include <chrono>
#include <cstdlib>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

void runBody(harness::TestContext& ctx)
{
    // 3 = control(0) + "inv" + "orders".
    harness::formatSession(3, 4);

    auto owner = harness::openSession();
    // The owner (peer 0, control genesis holder) creates the data domains.
    owner.createDomain("inv");
    owner.createDomain("orders");
    auto joiner = harness::openSession();

    // ── opt-in: a domain is not joined by default ─────────────────
    // The joiner has not joined "inv" -> locking it is refused.
    const bool lockWithoutJoinRefused = harness::threw<cme::NotParticipatingError>(
        [&]
        {
            (void)joiner.lock("inv");
        });
    if (!ctx.check(lockWithoutJoinRefused, "lock without join must throw NotParticipatingError"))
    {
        return;
    }

    // Join, then lock works. Both sessions join "orders" (two participants).
    owner.joinDomain("inv");
    owner.joinDomain("orders");
    joiner.joinDomain("orders");
    {
        auto guard = owner.lock("orders");
        if (!ctx.check(static_cast<bool>(guard), "lock(orders) after join"))
        {
            return;
        }
    }

    // ── leaveDomain -> lock refused on that domain ────────────────
    owner.leaveDomain("orders");  // the joiner still participates -> allowed
    const bool lockAfterLeaveRefused = harness::threw<cme::NotParticipatingError>(
        [&]
        {
            (void)owner.lock("orders");
        });
    if (!ctx.check(lockAfterLeaveRefused,
                   "lock(orders) after leave must throw NotParticipatingError"))
    {
        return;
    }
    if (!ctx.check(!owner.tryLock("orders", std::chrono::milliseconds{50}).has_value(),
                   "tryLock(orders) after leave must return nullopt"))
    {
        return;
    }
    // Other domains unaffected.
    {
        auto guard = owner.lock("inv");
        if (!ctx.check(static_cast<bool>(guard), "lock(inv) still works after leaving orders"))
        {
            return;
        }
    }

    // ── joinDomain re-enables lock ────────────────────────────────
    owner.joinDomain("orders");
    {
        auto guard = owner.lock("orders");
        if (!ctx.check(static_cast<bool>(guard), "lock(orders) works again after rejoin"))
        {
            return;
        }
    }
    // Idempotent: joining again is a no-op.
    owner.joinDomain("orders");

    // ── sole-participant leave refused ────────────────────────────
    // Drop the joiner from orders, leaving the owner the only participant; the owner then
    // cannot leave (the domain would be orphaned -- deleteDomain is the only exit).
    joiner.leaveDomain("orders");
    const bool soleParticipantLeaveRefused = harness::threw<cme::NotParticipatingError>(
        [&]
        {
            owner.leaveDomain("orders");
        });
    if (!ctx.check(soleParticipantLeaveRefused,
                   "sole-participant leave(orders) must throw NotParticipatingError"))
    {
        return;
    }
    // The owner still holds participation, so lock still works.
    {
        auto guard = owner.lock("orders");
        if (!ctx.check(static_cast<bool>(guard), "sole participant can still lock"))
        {
            return;
        }
    }

    // ── unknown domain ────────────────────────────────────────────
    const bool unknownJoinRefused = harness::threw<cme::UnknownDomainError>(
        [&]
        {
            owner.joinDomain("ghost");
        });
    if (!ctx.check(unknownJoinRefused, "joinDomain(ghost) must throw UnknownDomainError"))
    {
        return;
    }
    const bool unknownLeaveRefused = harness::threw<cme::UnknownDomainError>(
        [&]
        {
            owner.leaveDomain("ghost");
        });
    if (!ctx.check(unknownLeaveRefused, "leaveDomain(ghost) must throw UnknownDomainError"))
    {
        return;
    }
}

}  // namespace

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
