// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_slot_inherit.cpp -- a member slot's next occupant inherits nothing from the last one.
//
// leaveMembership clears participation in the same 64 B write that sets Status::None, and the
// comment there says why: admission reserves a slot by stamping Active without touching
// participation, so a data bit left behind would let a holder grant to the next occupant
// before that peer has synced its view of the domain.
//
// readmit_gate covers the dead peer's slot, where recovery is what clears it. The clean-leave
// path reaches the same bits by a different route and had no case.
//
// The tenant joins two domains so a partial clear shows up as one bit surviving rather than as
// nothing happening at all. The newcomer takes the same slot and is asked for the domain
// directly, since a peer that participates in nothing must be refused rather than served.
//
// Peer rather than Session: the whole claim is about one member slot, and the case has to read
// that slot by index and hand the same index to the next occupant. Admission chooses where a
// Session lands and reports it nowhere.

#include <chrono>
#include <cstdint>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

// Slot 0 is control, slots 1 and 2 are the two domains the tenant joins.
constexpr std::uint32_t FormatDomains = 3;
constexpr std::uint32_t FormatPeers = 2;

constexpr cme::PeerId HolderId = 0;
constexpr cme::PeerId TenantId = 1;

constexpr const char* FirstDomain = "lane0";
constexpr const char* SecondDomain = "lane1";

constexpr std::chrono::milliseconds GrantWindow{3'000};

[[nodiscard]] bool participatesIn(const cme::Geometry& region, cme::PeerId peerId,
                                  cme::DomainId domainId, cme::CoherencyMode coherency)
{
    return harness::readMemberSlot(region, peerId, coherency).loadParticipatingDomains().has(domainId);
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();
    auto region = harness::createRegion(ctx, FormatDomains, FormatPeers);

    cme::Peer holder{region, HolderId, coherency};
    const cme::DomainId firstLane = holder.createDomain(FirstDomain);
    const cme::DomainId secondLane = holder.createDomain(SecondDomain);

    {
        cme::Peer tenant{region, TenantId, coherency};
        tenant.joinDomain(firstLane);
        tenant.joinDomain(secondLane);
        ctx.check(participatesIn(region, TenantId, firstLane, coherency) &&
                      participatesIn(region, TenantId, secondLane, coherency),
                  "the tenant's slot carries both domains while it is joined");
    }

    // The destructor is the clean-leave path: Leaving, drain, then None with participation
    // cleared in the same write.
    ctx.check(harness::hasMemberStatus(region, TenantId,
                                       cme::Geometry::Member_t::Status::None, coherency),
              "the departed slot is None");
    ctx.check(!participatesIn(region, TenantId, firstLane, coherency) &&
                  !participatesIn(region, TenantId, secondLane, coherency),
              "the departed slot carries no data domain");
    ctx.check(!participatesIn(region, TenantId, cme::ControlDomainId, coherency),
              "the departed slot carries no control domain either");

    cme::Peer newcomer{region, TenantId, coherency};
    ctx.check(participatesIn(region, TenantId, cme::ControlDomainId, coherency),
              "the new occupant is seeded with the control domain");
    ctx.check(!participatesIn(region, TenantId, firstLane, coherency) &&
                  !participatesIn(region, TenantId, secondLane, coherency),
              "the new occupant inherits no data domain");

    const auto lockUnjoined = [&newcomer, firstLane]
    {
        static_cast<void>(newcomer.lock(firstLane));
    };
    ctx.check(harness::threw<cme::NotParticipatingError>(lockUnjoined),
              "the new occupant cannot be granted a domain it has not joined");

    // Joining is what makes the same call work, so the refusal above belongs to participation
    // rather than to a peer that cannot acquire anything.
    newcomer.joinDomain(firstLane);
    ctx.check(newcomer.tryLock(firstLane, GrantWindow).has_value(),
              "the new occupant acquires the domain once it joins");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
