// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_participation.cpp -- Phase 2 opt-in participation API.
//
// A session locks a domain only after joining it, and createDomain joins its creator. So the
// guard is driven by a second session that never joined, and by leaving and re-joining. Covers:
//   - no join -> lock refused
//   - joinDomain enables lock (re-baseline + re-advertise)
//   - leaveDomain -> lock / tryLock on that domain now refused
//   - sole-participant leave is refused (would orphan the domain)
//   - joinDomain idempotent: the slot's participation bitmap is unchanged
//   - unknown domain name -> UnknownDomainError
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.

#include <cstdint>
#include <cstdlib>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/domain_bitmap.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

// The owner opens first on a fresh region, so admission gives it the lowest slot. The idempotence
// check reads that slot, and a wrong index shows up as the whole expected set failing to match.
constexpr cme::PeerId OwnerPeerId = 0;

// Whether the two bitmaps hold the same domain ids. Bits carries no operator==, and a stray bit
// anywhere in the words is what a broken idempotence writes, so every word is compared.
[[nodiscard]] bool sameDomainSet(const cme::DomainBitmap& left, const cme::DomainBitmap& right)
{
    for (std::uint32_t word = 0; word < cme::DomainWordCount; ++word)
    {
        if (left.getWord(word) != right.getWord(word))
        {
            return false;
        }
    }
    return true;
}

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
    if (!ctx.check(!harness::canLock(owner, "orders", timing::Millis{50}),
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
    // Idempotent: a second join leaves the slot's participation set exactly as it stands. Against
    // the whole expected set rather than a before/after diff, because a stray bit raised on every
    // idempotent call stands before the second one too and a diff would not see it.
    auto region = harness::openBoundRegion();
    cme::DomainBitmap expected;
    expected.set(cme::ControlDomainId);  // seeded at admission, so it is part of the set
    expected.set(owner.resolveDomain("inv").id);
    expected.set(owner.resolveDomain("orders").id);
    if (!ctx.check(sameDomainSet(harness::readMemberSlot(region, OwnerPeerId).loadParticipatingDomains(),
                                 expected),
                   "the owner's slot carries the control domain, inv and orders, and nothing else"))
    {
        return;
    }
    owner.joinDomain("orders");
    if (!ctx.check(sameDomainSet(harness::readMemberSlot(region, OwnerPeerId).loadParticipatingDomains(),
                                 expected),
                   "a second joinDomain(orders) leaves that set unchanged"))
    {
        return;
    }

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
