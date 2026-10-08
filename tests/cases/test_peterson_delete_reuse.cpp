// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_peterson_delete_reuse.cpp -- a domain created in the slot a deleted one freed is lockable by
// another peer under Peterson.
//
// Delete takes the domain's lock, so the deleting peer climbs the slot's tournament and wins it. The
// tournament is per slot rather than per domain, so whatever interest that climb leaves behind is what
// the next domain in the slot starts with. One data slot forces the reuse.

#include <cstdint>

#include "common/timing.hpp"
#include "core/types.hpp"
#include "helper_cme.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr std::uint32_t FormatDomains = 2;  // control + the one data slot both domains take
constexpr std::uint32_t FormatPeers = 2;

constexpr cme::PeerId CreatorId = 0;
constexpr cme::PeerId WaiterId = 1;

// Well past one acquire budget, so only an interest nobody will retract refuses the lock.
constexpr timing::Millis GrantWindow{3'000};

}  // namespace

void runBody(harness::TestContext& ctx)
{
    auto region = harness::createRegion(FormatDomains, FormatPeers);
    auto creator = harness::makePeer(region, CreatorId);
    auto waiter = harness::makePeer(region, WaiterId);

    const auto first = creator.createDomain("first");
    creator.joinDomain(first.id);
    creator.deleteDomain(first.id);

    const auto second = creator.createDomain("second");
    if (!ctx.check(second.id == first.id, "the second domain takes the slot the first one freed"))
    {
        return;
    }

    waiter.joinDomain(second.id);
    const auto granted = waiter.tryLock(second.id, GrantWindow);
    ctx.check(granted.has_value(), "another peer locks the domain in the reused slot");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
