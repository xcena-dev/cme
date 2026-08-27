// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_failpoint_admission.cpp -- a joiner killed inside claimPeerSlot, on either side of the slot.
//
// Both boundaries are Mechanism: JoinMembership is one atomic Free -> Member in the spec, which has
// no lease and cannot express either state. lease_steal writes the stalled nonce by hand, which
// proves the steal but not that a crash leaves that line. A region per check, as region_corrupt
// does: the residue of the first would decide the second.

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <exception>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "config.hpp"
#include "core/layout/geometry.hpp"
#include "helper.hpp"
#include "observe/failpoint.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

// The two boundaries this file is about.
constexpr auto BeforeCommit = cme::failpoint::Boundary::AdmissionBeforeCommit;
constexpr auto AfterCommit = cme::failpoint::Boundary::AdmissionAfterCommit;

constexpr std::uint32_t MaxDomains = 2;
constexpr std::uint32_t MaxPeers = 2;

// Liveness grace plus takeover, for the slot the second check leaves behind.
constexpr std::uint32_t RecoveryDeadlineMs = 20000;

// Named in the unit both uses want, so a change of the constant's unit does not make the printed
// label lie.
constexpr auto LeaseTimeoutMs =
    std::chrono::duration_cast<timing::Millis>(cme::LeaseTimeout);

// What a child does to reach either boundary: both sit inside claimPeerSlot, which the open runs
// on its way in, so the child never gets past this line.
constexpr auto OpenAndDie = []
{
    auto session = harness::openSession();
    static_cast<void>(session);
};

// ── the lease, with no slot behind it ───────────────────────────────
// The crash leaves a nonce nobody will clear. The next joiner has to wait it out and steal it,
// which is the only route in.
void checkKilledHoldingLease(harness::TestContext& ctx)
{
    harness::formatSession(MaxDomains, MaxPeers);
    if (!ctx.check(harness::killChildAt(BeforeCommit, OpenAndDie),
                   "the joiner died holding the lease rather than finishing"))
    {
        return;
    }

    const timing::Stopwatch joining;
    bool joined = false;
    try
    {
        auto session = harness::openSession();
        session.createDomain("after");
        joined = harness::listsDomain(session, "after");
    }
    catch (const std::exception& error)
    {
        std::printf("the joiner after the crash failed: %s\n", error.what());
    }
    const auto waited = joining.elapsed<timing::Millis>();

    std::printf("%s: the next joiner got in after %" PRId64 " ms (LeaseTimeout %" PRId64 " ms)\n",
                cme::failpoint::readName(BeforeCommit), waited.count(), LeaseTimeoutMs.count());

    ctx.check(joined, "a later joiner got past the lease the crash left behind");
    ctx.check(waited >= LeaseTimeoutMs,
              "and only after LeaseTimeout, so it stole the lease rather than finding it free");
}

// ── the slot, with the lease still on it ────────────────────────────
// Worse than the one above: the slot reads Active, so every peer treats it as a live member, and
// nothing will ever heartbeat it. The lease has to be stolen and the slot has to be recovered.
void checkKilledHoldingSlot(harness::TestContext& ctx)
{
    harness::formatSession(MaxDomains, MaxPeers);
    if (!ctx.check(harness::killChildAt(AfterCommit, OpenAndDie),
                   "the joiner died holding the slot rather than finishing"))
    {
        return;
    }
    std::printf("%s: the slot is committed and the lease is still on it\n",
                cme::failpoint::readName(AfterCommit));

    auto region = harness::openBoundRegion();
    ctx.check(harness::hasMemberStatus(region, 0, Status::Active),
              "the crash left a slot that reads Active");

    bool joined = false;
    try
    {
        auto session = harness::openSession();
        joined = true;
    }
    catch (const std::exception& error)
    {
        std::printf("the joiner after the crash failed: %s\n", error.what());
    }
    ctx.check(joined, "a later joiner still got a slot of its own");

    // The phantom never answers, so liveness has to declare it and recovery has to free it. Held
    // open by a live peer, since a region with nobody in it has no recovery authority.
    auto survivor = harness::openSession();
    const bool freed = harness::waitUntil(
        [&region]
        {
            return harness::hasMemberStatus(region, 0, Status::None);
        },
        RecoveryDeadlineMs);
    ctx.check(freed, "and the slot nothing was heartbeating was recovered");
    static_cast<void>(survivor);
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so nothing would be killed");
    }

    checkKilledHoldingLease(ctx);
    checkKilledHoldingSlot(ctx);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
