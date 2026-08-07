// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_degenerate_region.cpp -- the region with one peer in it.
//
// No case formats maxPeers = 1, and that shape is where the arithmetic edges live:
// getGroupCount(1), a peer ring with nobody else in it, a successor scan that must find no
// candidate and leave the domain where it is, and RequestAgg's group count against a single peer.
// api_reject already found one guard on this boundary, aggregatorGroups > peerCount.
//
// All four policies, because what differs at peerCount = 1 is the successor policy: the ring walk,
// the grant scan and the tournament tree each reduce to a degenerate case of their own.
//
// The domain is created, locked, released and locked again. The second acquire is the one that
// matters: it comes after a release with no other peer to hand the domain to, so whatever the
// policy left behind has to be a state the same peer can come back from. What that state is
// differs by policy, and the assertion at the end says how.

#include <cstdint>

#include "cme/errors.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

// Slot 0 is control, slot 1 is the only data domain, and there is one member slot.
constexpr std::uint32_t FormatDomains = 2;
constexpr std::uint32_t FormatPeers = 1;

constexpr cme::PeerId SoloId = 0;
constexpr const char* Domain = "lane0";

}  // namespace

void runBody(harness::TestContext& ctx)
{
    auto region = harness::createRegion(FormatDomains, FormatPeers);

    ctx.checkf(cme::Geometry::getGroupCount(FormatPeers) == 1, "one peer is one shadow group");
    ctx.checkf(cme::Geometry::getRecordsPerDomain(FormatPeers) == 2,
               "one truth copy and one shadow per domain");

    auto solo = harness::makePeer(region, SoloId);
    const cme::DomainId lane = solo.createDomain(Domain);
    if (!ctx.check(!cme::isNoDomain(lane), "the sole peer creates a domain"))
    {
        return;
    }

    {
        const auto guard = solo.lock(lane);
        ctx.check(static_cast<bool>(guard), "the sole peer locks the domain it created");
    }

    // The release above had nobody to hand the domain to, and this acquire is what proves the
    // release left a state the sole peer can come back from.
    {
        const auto guard = solo.lock(lane);
        ctx.check(static_cast<bool>(guard), "the sole peer re-locks after releasing");
    }

    // Two shapes are correct here and which one appears is the policy's choice. Order and the
    // Request family keep a definite holder, so the record still names the peer. Peterson vacates
    // on release -- it is the only policy that can seize a holderless record, so leaving it held
    // would be the bug there. What no policy may do is name somebody else: this region has no
    // somebody else.
    const auto holder = harness::readDomainRecord(region, lane).getHolder();
    ctx.checkf(holder == SoloId || cme::isNoPeer(holder),
               "the record names the sole peer or nobody, not a third party (holder=%u)", holder);

    // Leaving would orphan the domain, so the only exit is deleting it.
    const auto leaveDomain = [&solo, lane]
    {
        solo.leaveDomain(lane);
    };
    ctx.check(harness::threw<cme::NotParticipatingError>(leaveDomain),
              "the sole participant cannot leave the domain");

    solo.deleteDomain(lane);
    ctx.check(solo.resolveDomainName(Domain) == cme::NoDomain, "the domain is gone after delete");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
