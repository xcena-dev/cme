// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_api_contract.cpp -- the declared contracts no scenario case happens to exercise.
//
// Two of them, unrelated to each other and here together because neither earns a file of its own.
//
// Move assignment on Session and Guard. Both are declared in shared.hpp, so a caller may write
// them, and no case did: every case builds its objects where it uses them. Guard is the one that
// matters, because assigning over a live Guard has to release the domain the target was holding
// before it takes the source's, and getting that wrong strands a domain rather than failing loudly.
//
// The successor-policy factory. makeSuccessorPolicy maps a Strategy to a policy and kind() maps
// back, and until now nothing compared the two: kind() was never called on any of the four. A
// policy header copied from its neighbour with kind() left pointing at the original would pass
// every other case in the tree, because a region formatted with one strategy and read with one
// impl agrees with itself.

#include <cstdint>
#include <memory>
#include <utility>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/policy/successor.hpp"
#include "core/policy/successor_policy.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "test_options.hpp"

namespace test
{
namespace
{

constexpr const char* Domain = "lane0";
constexpr const char* OtherDomain = "lane1";  // the guard move-assign needs two held at once

// 3 = control(0) + the two data domains the guard case holds at once.
constexpr std::uint32_t FormatDomains = 3;
constexpr std::uint32_t FormatPeers = 4;

// How long the observing peer is given to take the domain the assignment released. Wide enough for
// the holder's poll cycle to carry the grant across.
constexpr timing::Millis ReleaseWindow{3'000};

// Every strategy the public enum offers. Kept here rather than derived from a count, so adding a
// kind to the enum without teaching the factory about it fails this case instead of being skipped.
constexpr cme::Strategy AllStrategies[] = {
    cme::Strategy::Order,
    cme::Strategy::Request,
    cme::Strategy::RequestAgg,
    cme::Strategy::Peterson,
};

void checkFactoryRoundTrip(harness::TestContext& ctx)
{
    for (const cme::Strategy requested : AllStrategies)
    {
        const std::unique_ptr<cme::SuccessorPolicy> policy = cme::makeSuccessorPolicy(requested);
        if (!ctx.checkf(policy != nullptr, "makeSuccessorPolicy(%s) returns a policy",
                        harness::strategyName(requested)))
        {
            continue;  // kind() on a null policy would take the run down with it
        }
        const cme::Strategy reported = policy->kind();
        ctx.checkf(reported == requested, "%s: the policy reports its own kind (got %s)",
                   harness::strategyName(requested), harness::strategyName(reported));
    }
}

void checkSessionMoveAssign(harness::TestContext& ctx)
{
    harness::formatSession(FormatDomains, FormatPeers);

    auto source = harness::openSession();
    source.createDomain(Domain);

    // The target is a live session, not a default-constructed one: Session has no default ctor,
    // and overwriting a live object is the case that has to give the old peer slot back.
    auto target = harness::openSession();
    target = std::move(source);

    // The domain was created through the object now moved from, and participation travelled with
    // the Impl rather than staying with the name.
    const auto guard = target.lock(Domain);
    ctx.check(static_cast<bool>(guard), "session move assignment: the target locks the moved domain");
}

void checkGuardMoveAssign(harness::TestContext& ctx)
{
    harness::formatSession(FormatDomains, FormatPeers);

    auto session = harness::openSession();
    session.createDomain(Domain);
    session.createDomain(OtherDomain);

    // A second peer, because the release the assignment owes is not observable from the peer that
    // held the guard: its own token makes a re-acquire of its own domain a resident fast path.
    auto observer = harness::openSession();
    observer.joinDomain(Domain);

    // Two shapes, because what the target already holds decides what the assignment has to do.
    // An empty target only takes the source's domain. A live one gives its own back first.
    {
        cme::Guard deferred;
        ctx.check(!static_cast<bool>(deferred), "a default-constructed Guard holds nothing");
        deferred = session.lock(Domain);
        ctx.check(static_cast<bool>(deferred), "assigning into an empty Guard holds the domain");
    }

    {
        auto holder = session.lock(Domain);
        auto donor = session.lock(OtherDomain);

        // The moved-from Guard goes unasserted. Guard holds one unique_ptr and its move operations
        // are defaulted, so an implementation that copied instead of transferring would not
        // compile: a check for it could not fail, and a check that cannot fail is not one.
        holder = std::move(donor);
        ctx.check(static_cast<bool>(holder), "guard move assignment: the target holds a domain");

        // The overwritten domain has to be free while the target still holds the source's. An
        // assignment that swaps the pointer without destroying the pointee leaves it pinned, and
        // the observer's deadline is the only place that shows.
        const auto observed = observer.tryLock(Domain, ReleaseWindow);
        ctx.check(observed.has_value(),
                  "guard move assignment: another peer takes the overwritten domain");
    }

    // Both Guards are gone now. A release that ran twice or not at all shows up here: the first
    // leaves a domain unowned mid-transfer, the second leaves it pinned.
    const auto reacquired = session.lock(Domain);
    ctx.check(static_cast<bool>(reacquired), "after the assignment: the overwritten domain locks");
    const auto other = session.lock(OtherDomain);
    ctx.check(static_cast<bool>(other), "after the assignment: the moved domain locks");
}

// Peer is internal, so it needs a Geometry rather than a Session, and it is the one type here
// whose move has something at stake. Peer owns a poll thread that holds a reference into its
// LocalPeerState. The state sits behind a unique_ptr, so a move hands over the pointer and leaves
// the pointee where it was, which is what keeps that reference valid. A lock through the target
// afterwards is what shows the thread and the policy came across with it.
void checkPeerMove(harness::TestContext& ctx)
{
    constexpr cme::DomainId NumDomains = 2;  // control(0) + the one lane below
    constexpr cme::PeerId MaxPeers = 4;

    auto region = cme::Geometry::create(ctx.uri(), NumDomains, MaxPeers,
                                        cme::Geometry::FormatOpts_t{ctx.strategy()});

    auto origin = harness::makePeer(region, 0);
    const cme::DomainId lane = origin.createDomain("lane1").id;
    origin.joinDomain(lane);

    cme::Peer moved{std::move(origin)};
    ctx.check(moved.getPeerId() == 0, "peer move construction: the target keeps the peer id");
    {
        const auto guard = moved.lock(lane);
        ctx.check(static_cast<bool>(guard), "peer move construction: the target still locks");
    }

    // The target is a live peer holding its own slot, so the assignment has to leave and destroy
    // that one before it takes the source's.
    auto target = harness::makePeer(region, 1);
    ctx.check(target.getPeerId() == 1, "a second peer holds its own slot");

    target = std::move(moved);
    ctx.check(target.getPeerId() == 0, "peer move assignment: the target takes the source's id");
    {
        const auto guard = target.lock(lane);
        ctx.check(static_cast<bool>(guard), "peer move assignment: the target still locks");
    }
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    // No region needed: the factory builds a policy that has not been bound to one yet.
    checkFactoryRoundTrip(ctx);

    // Each of these formats the region again, so none inherits another's peer slots.
    checkSessionMoveAssign(ctx);
    checkGuardMoveAssign(ctx);
    checkPeerMove(ctx);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
