// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_shadow_authority.cpp -- the group shadow is a cost filter; the truth line decides.
//
// §9.4 and the R1 requirement row state that a waiter polls its group's shadow copy of the
// domain record only to spread the read load, and confirms against the truth copy before it
// acts. waitForOwnership implements exactly that: an epoch edge on the shadow triggers one
// truth read, and the truth's holder field alone settles the acquire.
//
// No other case writes or reads a shadow. publishDomainRecord writes the shadow and the truth
// from one site, so every other scenario keeps the two in agreement, and a build that adopted
// the shadow directly would pass the whole suite.
//
// The injection is a shadow naming the waiter at an epoch above the truth's, with the truth
// left naming the holder. The holder keeps a guard out across the window, because
// canTransferDomain refuses a grant while a domain is pinned: without that, an acquire here
// could succeed for the ordinary reason and prove nothing.
//
// Peer rather than Session, as in region_corrupt: the shadow to forge is the waiter's own, and
// getDomainRecordShadow takes a peer index. Admission picks the slot a Session lands on and
// reports it nowhere, so the id has to be named by the case.

#include <chrono>
#include <cstdint>

#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

// Slot 0 is control, slot 1 is the domain both peers work on.
constexpr std::uint32_t FormatDomains = 2;
constexpr std::uint32_t FormatPeers = 2;

constexpr cme::PeerId HolderId = 0;
constexpr cme::PeerId WaiterId = 1;

constexpr const char* Domain = "lane0";

// Clear of every epoch the run reaches, so the forged shadow passes waitForOwnership's
// freshness filter and the truth read is the only thing left to refuse it.
constexpr std::uint64_t EpochLead = 16;

// The window the waiter is given while the shadow lies. A shadow-trusting build adopts on its
// first poll, so this only has to outlast the poll interval.
constexpr std::chrono::milliseconds ForgedWindow{400};

// The window for the real handoff once the holder unpins. Wide enough for one poll cycle to
// carry the grant across.
constexpr std::chrono::milliseconds GrantWindow{3'000};

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();
    auto region = harness::createRegion(ctx, FormatDomains, FormatPeers);

    cme::Peer holder{region, HolderId, coherency};
    cme::Peer waiter{region, WaiterId, coherency};
    const cme::DomainId lane = holder.createDomain(Domain);
    waiter.joinDomain(lane);

    auto held = holder.lock(lane);
    if (!ctx.check(static_cast<bool>(held), "the holder pins its own domain"))
    {
        return;
    }

    const auto truth = harness::readDomainRecord(region, lane, coherency);
    if (!ctx.checkf(truth.getHolder() == HolderId, "the truth names peer %u", HolderId))
    {
        return;
    }

    auto forged = truth;
    forged.holder = WaiterId;
    forged.epoch = truth.epoch + EpochLead;
    cme::coherency::set(region.getDomainRecordShadow(lane, WaiterId), forged, coherency);

    ctx.check(!waiter.tryLock(lane, ForgedWindow).has_value(),
              "a shadow naming the waiter does not hand it the domain");
    ctx.check(harness::readDomainRecord(region, lane, coherency).getHolder() == HolderId,
              "the truth still names the holder after the refused acquire");

    // The same acquire has to succeed once the truth agrees, or the refusal above would also be
    // what a peer that cannot acquire at all looks like.
    held.release();
    const auto granted = waiter.tryLock(lane, GrantWindow);
    ctx.check(granted.has_value(), "the waiter acquires once the truth names it");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
