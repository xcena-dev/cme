// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_ceiling_region.cpp -- a region formatted at MaxDomains x MaxPeers.
//
// The report measures the 64 x 63 corner, so the code runs there, but no ctest case formats at the
// ceiling. region_reject raises the header dims past it only to make the computed area exceed the
// mapping, which is the opposite check: that one asserts the refusal, this one asserts the fit.
//
// What the ceiling exercises is the layout arithmetic and the shadow indexing at their largest
// values. The waiter below is the last member slot precisely so that its shadow is the last copy
// in the per-domain record block.
//
// Registered on every medium rather than shm alone. The arithmetic is medium-independent, but
// whether the area fits is not: a devdax window is 2 MiB and a uc file is sized by its creator, so
// each medium answers a different question about the same dimensions.
//
// Two peers, not 64. What is under test is the size of the region, not the number of participants,
// and 64 poll threads would make this one of the slowest cases in the suite for nothing.

#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr cme::PeerId HolderId = 0;
// The last member slot, so getGroupIndex lands on the final shadow of the block.
constexpr cme::PeerId WaiterId = cme::MaxPeers - 1;

constexpr const char* Domain = "lane0";

constexpr timing::Millis GrantWindow{3'000};

}  // namespace

void runBody(harness::TestContext& ctx)
{
    auto region = harness::createRegion(cme::MaxDomains, cme::MaxPeers);

    ctx.checkf(region.getDomainCount() == cme::MaxDomains && region.getPeerCount() == cme::MaxPeers,
               "the region formats at %u domains x %u peers", cme::MaxDomains, cme::MaxPeers);
    // The sizing function against the indexing one, rather than either against a literal: the copy
    // the last peer polls has to sit inside the block the same dimensions sized.
    const auto copies = cme::Geometry::getRecordsPerDomain(cme::MaxPeers);
    const auto lastShadow = cme::Geometry::getGroupIndex(WaiterId) + 1;  // + the truth at index 0
    ctx.checkf(lastShadow < copies, "the last peer's shadow is copy %u of %u per domain", lastShadow,
               copies);

    auto holder = harness::makePeer(region, HolderId);
    auto waiter = harness::makePeer(region, WaiterId);
    const cme::DomainId lane = holder.createDomain(Domain).id;
    waiter.joinDomain(lane);

    const auto granted = waiter.tryLock(lane, GrantWindow);
    if (!ctx.check(granted.has_value(), "the handoff completes at the ceiling"))
    {
        return;
    }

    // The truth is the authority, and the holder's own shadow has to agree with it: publish writes
    // the holder-group shadow first and the truth second, from one site.
    ctx.check(harness::readDomainRecord(region, lane).isHeldBy(WaiterId),
              "the truth names the waiter after the handoff");
    ctx.check(harness::readDomainRecordShadow(region, lane, WaiterId).isHeldBy(WaiterId),
              "the last shadow in the block names it too");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
