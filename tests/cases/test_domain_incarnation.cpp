// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_domain_incarnation.cpp -- a reused domain slot is a new incarnation, not the old one.
//
// createDomainLocked bumps generation on the slot it reuses and leaves epoch alone, and the
// comments name what each field is for: generation is the slot-reuse ABA guard, and a monotonic
// epoch is what lets prior holders still tell a newer transfer from an older one.
//
// domain_lifecycle covers slot reuse by name and by count, so the reuse itself runs. Nothing
// checked the discrimination the two fields exist for: generation appears in tests/cases only
// in test_util.cpp, at the primitive level.
//
// The region has one data slot, so the second create has to land on the first one's bytes. The
// tenant acquires the first incarnation before it leaves, which is what puts a real epoch in
// its local view: the last assertion is that the same peer can still acquire the new
// incarnation afterwards, and a reset epoch would leave its baseline above the record forever.
//
// Peer rather than Session: the subject is a slot rather than a name, and only the Peer API
// takes a DomainId. Session resolves a name to whatever slot currently carries it, which is the
// indirection this case exists to look behind.

#include <chrono>
#include <cinttypes>
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

// Slot 0 is control and slot 1 is the only data slot, so a reuse cannot land anywhere else.
constexpr std::uint32_t FormatDomains = 2;
constexpr std::uint32_t FormatPeers = 2;

constexpr cme::PeerId HolderId = 0;
constexpr cme::PeerId TenantId = 1;

constexpr const char* FirstName = "alpha";
constexpr const char* SecondName = "beta";

constexpr std::chrono::milliseconds GrantWindow{3'000};

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();
    auto region = harness::createRegion(ctx, FormatDomains, FormatPeers);

    cme::Peer holder{region, HolderId, coherency};
    cme::Peer tenant{region, TenantId, coherency};

    const cme::DomainId lane = holder.createDomain(FirstName);
    tenant.joinDomain(lane);
    if (!ctx.check(tenant.tryLock(lane, GrantWindow).has_value(),
                   "the tenant acquires the first incarnation"))
    {
        return;
    }

    const auto before = harness::readDomainRecord(region, lane, coherency);

    // The tenant has to be out before the delete: deleteDomainLocked refuses while another peer
    // participates, and the holder has to take the record back to run it at all.
    tenant.leaveDomain(lane);
    holder.deleteDomain(lane);
    ctx.check(!harness::readDomainRecord(region, lane, coherency)
                   .hasState(cme::Geometry::DomainRecord_t::State::Active),
              "the deleted slot is no longer Active");

    const cme::DomainId reused = holder.createDomain(SecondName);
    if (!ctx.checkf(reused == lane, "the new domain lands on slot %u again", lane))
    {
        return;
    }

    const auto after = harness::readDomainRecord(region, lane, coherency);
    const auto beforeGeneration = static_cast<std::uint64_t>(before.generation);
    const auto afterGeneration = static_cast<std::uint64_t>(after.generation);
    const auto beforeEpoch = static_cast<std::uint64_t>(before.epoch);
    const auto afterEpoch = static_cast<std::uint64_t>(after.epoch);
    ctx.checkf(afterGeneration > beforeGeneration,
               "generation rises on reuse (%" PRIu64 " -> %" PRIu64 ")", beforeGeneration,
               afterGeneration);
    ctx.checkf(afterEpoch >= beforeEpoch, "epoch is not reset by reuse (%" PRIu64 " -> %" PRIu64 ")",
               beforeEpoch, afterEpoch);
    ctx.check(holder.resolveDomainName(FirstName) == cme::NoDomain,
              "the old name resolves to nothing");
    ctx.check(holder.resolveDomainName(SecondName) == lane,
              "the new name resolves to the reused slot");

    // The tenant still holds a view of the old incarnation, and the slot number it knows is now
    // somebody else's domain. Participation went with the leave and the create did not hand it
    // back, so the acquire is refused rather than served from that view.
    const auto lockStaleView = [&tenant, lane]
    {
        static_cast<void>(tenant.lock(lane));
    };
    ctx.check(harness::threw<cme::NotParticipatingError>(lockStaleView),
              "the tenant cannot act on the new incarnation with its old view");

    tenant.joinDomain(lane);
    ctx.check(tenant.tryLock(lane, GrantWindow).has_value(),
              "the tenant acquires the new incarnation after re-syncing");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
