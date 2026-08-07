// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_shared_lifecycle.cpp -- SharedSession's exit paths: leave, delete, and moving the object.
//
// test_shared_session covers the entry side, so it formats, creates one domain and hammers it.
// It never leaves and never deletes, which leaves SharedSession::leaveDomain, deleteDomain and
// the dropHeldOwnership both of them call with no caller at all.
//
// Those three exist for one invariant: a cohort holds the peer's CXL ownership across critical
// sections, so participation must not end while that ownership is still held. What this case
// asserts is the observable half -- after leaving, the domain is no longer lockable through this
// object, and after deleting, the name is gone from the region. The held-ownership-at-exit half
// needs a second thread queued on the tier mutex at the moment the last Guard dies, which is not
// reachable deterministically; releaseCohort hands ownership back whenever no local thread is
// waiting, and a single-threaded run always is that case.
//
// Also here because nothing else reaches them: the OpenOpts_t overload of open, Guard's move
// constructor, SharedSession's move assignment, and the public cme::flush.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "cme/shared_session.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr const char* LeaveDomain = "lane0";   // two participants, so leave is legal
constexpr const char* DeleteDomain = "lane1";  // sole participant, so delete is legal

// True when locking @name through @shared is refused because this object does not participate.
// Returns the verdict rather than asserting, so the caller words its own check line.
bool refusesLock(cme::SharedSession& shared, const char* name)
{
    try
    {
        const auto guard = shared.lock(name);
        return false;
    }
    catch (const cme::NotParticipatingError&)
    {
        return true;
    }
    catch (const cme::UnknownDomainError&)
    {
        // A deleted name reaches the Session layer as unknown rather than as non-participation.
        return true;
    }
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    // 4 domains = control(0) + the two data domains below, one spare.
    // 4 peers = shared, the verifier, and the move-assign target.
    harness::formatSession(4, 4);

    // Through the OpenOpts_t overload, since coherency has to match how this run reaches the
    // medium and the harness already resolved that from --backend.
    auto shared = harness::openSharedSession();

    shared.createDomain(LeaveDomain);
    shared.createDomain(DeleteDomain);

    // A plain Session, for the two questions SharedSession deliberately cannot answer: it owns
    // its Session and hands out no reference, so the domain list has to come from elsewhere.
    auto verifier = harness::openSession();
    verifier.joinDomain(LeaveDomain);

    // ── leaveDomain ────────────────────────────────────────────────────
    // Lock once first, so the tier has been through a full acquire and release before the exit
    // path runs rather than being untouched.
    {
        const auto guard = shared.lock(LeaveDomain);
        std::uint64_t payload = 0x5A;
        cme::flush(&payload, sizeof(payload), ctx.coherency());
    }
    shared.leaveDomain(LeaveDomain);
    ctx.check(refusesLock(shared, LeaveDomain),
              "after leaveDomain: lock on the left domain is refused");
    ctx.check(harness::listsDomain(verifier, LeaveDomain),
              "after leaveDomain: the domain itself still exists, only participation ended");

    // Rejoining reuses the tier the leave deliberately kept in the map.
    shared.joinDomain(LeaveDomain);
    {
        const auto guard = shared.lock(LeaveDomain);
        ctx.check(static_cast<bool>(guard), "after rejoin: lock succeeds again");
    }

    // ── Guard move construction ────────────────────────────────────────
    // The moved-from Guard must not run the cohort release, or the tier mutex is unlocked twice
    // and the next acquire is undefined. The lock that follows is what shows it ran once.
    {
        auto origin = shared.lock(LeaveDomain);
        auto moved = std::move(origin);
        // The moved-from Guard goes unasserted. Guard holds one unique_ptr and its move
        // constructor is defaulted, so an implementation that copied instead of transferring
        // would not compile: a check for it could not fail.
        ctx.check(static_cast<bool>(moved), "moved-to Guard holds the domain");
    }
    {
        const auto guard = shared.lock(LeaveDomain);
        ctx.check(static_cast<bool>(guard), "after a moved Guard died: the tier is lockable again");
    }

    // ── deleteDomain ───────────────────────────────────────────────────
    // This object created DeleteDomain and nobody joined it, so it is the sole participant,
    // which is what Session::deleteDomain requires.
    {
        const auto guard = shared.lock(DeleteDomain);
        ctx.check(static_cast<bool>(guard), "sole-participant domain locks before deletion");
    }
    shared.deleteDomain(DeleteDomain);
    ctx.check(!harness::listsDomain(verifier, DeleteDomain), "after deleteDomain: the name is gone");
    ctx.check(refusesLock(shared, DeleteDomain),
              "after deleteDomain: lock on the deleted domain is refused");

    // ── SharedSession move assignment ──────────────────────────────────
    // The target is opened first so the assignment overwrites a live object, which is the case
    // that has to release the old Impl rather than leak its peer slot.
    auto sink = harness::openSharedSession();
    sink = std::move(shared);
    {
        const auto guard = sink.lock(LeaveDomain);
        ctx.check(static_cast<bool>(guard), "after move assignment: the target still locks");
    }
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
