// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_failpoint_registry.cpp -- a peer killed partway through creating, deleting or reclaiming.
//
// All four are Mechanism. The spec's header puts create and delete out of scope, and the orphan
// sweep is not in it at all, so nothing but the implementation answers for what a crash leaves.
//
// What every check asserts is the same and is deliberately weak: the region is still usable. No
// stronger claim is available, because the spec makes none.

#include <cstdint>
#include <cstdio>
#include <exception>

#include "cme/shared.hpp"
#include "helper.hpp"
#include "observe/failpoint.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

// The four boundaries this file is about.
constexpr auto BeforeActivate = cme::failpoint::Boundary::CreateBeforeActivate;
constexpr auto BeforeFree = cme::failpoint::Boundary::DeleteBeforeFree;
constexpr auto BeforeDeactivate = cme::failpoint::Boundary::DeleteBeforeDeactivate;
constexpr auto MidReclaim = cme::failpoint::Boundary::ReclaimMidLoop;

// Room for the seeded domains plus the one each check makes, and three peers: the orphan a reclaim
// needs, the child that dies, and the watcher that opens afterwards.
constexpr std::uint32_t MaxDomains = 7;  // control + Seeded + the reclaim's two orphans + one more
constexpr std::uint32_t MaxPeers = 3;
constexpr std::uint32_t Seeded = 3;  // enough that a reclaim has a loop to stop halfway through

constexpr std::uint32_t RecoveryDeadlineMs = 20000;

// A region with @Seeded domains on it and nobody in it. The parent takes no slot until every armed
// child is reaped: ReclaimMidLoop sits on a poll thread, so any peer alive can reach it, and the
// parent is the one process that must not be the one to.
void freshRegion()
{
    harness::formatSession(MaxDomains, MaxPeers);
    auto region = harness::openBoundRegion();
    harness::seedDataDomains(region, Seeded);
}

// The region is usable when a survivor can still add a domain and take it. Weak on purpose: with
// create and delete outside the spec, "the registry did not wedge" is the whole claim.
[[nodiscard]] bool stillUsable(cme::Session& watcher, const char* name)
{
    try
    {
        watcher.createDomain(name);
        const auto guard = watcher.lock(name);
        static_cast<void>(guard);
    }
    catch (const std::exception& error)
    {
        std::printf("  the registry did not take a new domain: %s\n", error.what());
        return false;
    }
    return true;
}

// A child killed while creating: the record reads Active, the scan-scope bitmap does not.
void checkCreate(harness::TestContext& ctx)
{
    freshRegion();
    const bool died = harness::killChildAt(
        BeforeActivate,
        []
        {
            auto session = harness::openSession();
            session.createDomain("halfmade");
        });
    if (!ctx.check(died, "the creator died at its boundary"))
    {
        return;
    }
    auto watcher = harness::openSession();
    ctx.check(stillUsable(watcher, "after_create"),
              "a domain published but never activated leaves the registry usable");
}

// A child killed while deleting, on either side of the record write. @name is the domain the
// survivor then adds, kept apart from @what: a check's wording is longer than MaxNameLen.
void checkDelete(harness::TestContext& ctx, cme::failpoint::Boundary boundary, const char* name,
                 const char* what)
{
    freshRegion();
    const bool died = harness::killChildAt(
        boundary,
        []
        {
            auto session = harness::openSession();
            session.createDomain("doomed");
            session.deleteDomain("doomed");
        });
    if (!ctx.check(died, "the deleter died at its boundary"))
    {
        return;
    }
    std::printf("  %s\n", cme::failpoint::nameOf(boundary));
    auto watcher = harness::openSession();
    ctx.check(stillUsable(watcher, name), what);
}

// A child killed midway through the sweep that reclaims what a dead peer left. It has to leave
// orphans behind first, which _Exit does: no destructor, so its domains stay held by a corpse.
void checkReclaim(harness::TestContext& ctx)
{
    freshRegion();
    harness::reapChildren(
        harness::spawnChildren(
            1,
            [](std::uint32_t)
            {
                // Leaked, because _Exit skips only the destructors above this lambda: a session
                // that runs its own hands the domains back and leaves the sweep nothing to find.
                auto* session = new cme::Session{harness::openSession()};
                session->createDomain("orphan0");
                session->createDomain("orphan1");
            }));

    // The sweep runs on a live peer's poll thread, so the armed one has to be a peer of its own.
    const bool died = harness::killChildAt(
        MidReclaim,
        []
        {
            auto session = harness::openSession();
            harness::sleepMs(RecoveryDeadlineMs);
            static_cast<void>(session);
        });
    std::printf("  %s: the sweeper %s\n", cme::failpoint::nameOf(MidReclaim),
                died ? "died mid-loop" : "never reached the loop");
    if (!ctx.check(died, "the sweeper died partway through the reclaim"))
    {
        return;
    }
    auto watcher = harness::openSession();
    ctx.check(stillUsable(watcher, "after_reclaim"),
              "a reclaim that stopped halfway leaves the registry usable");
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so nothing would be killed");
    }

    checkCreate(ctx);
    checkDelete(ctx, BeforeFree, "after_free",
                "a delete that retracted participation and stopped leaves it usable");
    checkDelete(ctx, BeforeDeactivate, "after_deact",  // MaxNameLen is 16 and the test is >=
                "a delete that freed the record and stopped leaves it usable");
    checkReclaim(ctx);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
