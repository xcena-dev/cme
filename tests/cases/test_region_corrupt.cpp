// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_region_corrupt.cpp -- the slot-level magic checks, one cleared line at a time.
//
// test_region_reject covers the header: dims a joiner reads and refuses before it maps a layout
// over them. Everything below the header has its own check, and none of those had ever failed.
// Member_t, DomainRecord_t and the strategy field are each read by a different caller, so each
// gets its own injection here rather than one region corrupted every way at once.
//
// Why a cleared magic is the injection: format stamps magic last on every slot, so a zero there is
// what a peer sees when it maps a region another host is still formatting, or one a crashed
// formatter left half-written. The library's answer is to refuse rather than read fields whose
// layout it cannot confirm. State is left alone on purpose in the domain-record cases, because the
// Active check runs before the magic check and skipping it would test the wrong branch.
//
// Every check makes its own region. The injections are not independent -- a cleared member slot
// stays cleared, and admission would then hand the next opener a different slot -- so sharing one
// region would make each check depend on the order of the ones before it.
//
// Peer rather than Session throughout: admission skips a member slot whose magic is bad, so the
// join path that reads that slot is only reachable by naming the peer id directly.

#include <cstdint>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
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

// Slot 0 is control, slot 1 is the domain the join and lock cases work on.
constexpr std::uint32_t FormatDomains = 2;
constexpr std::uint32_t FormatPeers = 2;

constexpr const char* Domain = "lane0";

// A strategy value no makeSuccessorPolicy case matches. Out of the enum's range on purpose: the
// four real values are contiguous from zero, so a corrupt or newer-build region shows up here.
constexpr std::uint32_t UnknownStrategy = 99;

// Publish @slot with its magic zeroed. The whole 64 B line goes through coherency::set, which is
// how format commits it -- writing the one field in place would leave the rest unfenced on a
// noncoherent medium, and the failure would then say more about the write than about the check.
template <typename T_Slot>
void clearMagic(T_Slot* slot, cme::CoherencyMode coherency)
{
    auto line = cme::coherency::get(slot, coherency);
    line.magic = 0;
    cme::coherency::set(slot, line, coherency);
}

// ── the joiner's own member slot ────────────────────────────────────
// joinMembership reads this slot once, to seed the local copy it then writes through. A bad magic
// there means the write-through would land on bytes of unknown layout, so the join stops before it
// publishes anything and the peer never reaches Active.
void checkMemberSlotCleared(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();
    auto region = harness::createRegion(FormatDomains, FormatPeers);
    clearMagic(region.getMemberSlot(0), coherency);

    const auto joinRegion = [&region, coherency]
    {
        const auto joiner = harness::makePeer(region, 0);
    };
    ctx.check(harness::threw<cme::JoinError>(joinRegion),
              "cleared member magic: the join fails");
}

// ── the control domain's record ─────────────────────────────────────
// A joiner baselines every domain before it flips to Active, so it reads all numDomains records
// including control. That loop is the second place the join can stop, and it is separate from the
// member check: the member slot is this peer's alone, while a bad domain record means the region's
// shared registry is unreadable.
void checkControlRecordCleared(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();
    auto region = harness::createRegion(FormatDomains, FormatPeers);
    clearMagic(region.getDomainRecord(cme::ControlDomainId), coherency);

    const auto joinRegion = [&region, coherency]
    {
        const auto joiner = harness::makePeer(region, 0);
    };
    ctx.check(harness::threw<cme::JoinError>(joinRegion),
              "cleared control record magic: the join fails");
}

// ── a data domain's record, read by a peer joining it ───────────────
// The joiner baselines the domain from the record before it advertises participation, so a record
// it cannot read means it would forward from a view it never established. The state field is left
// Active by the injection, which is what lets the check under the control lock pass and the
// baseline read be the one that refuses.
void checkJoinRecordCleared(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();
    auto region = harness::createRegion(FormatDomains, FormatPeers);

    // Both peers join while the region is intact, so the failure below belongs to joinDomain rather
    // than to either join. The holder stays live: it owns control, and the bystander's joinDomain
    // has to take that lock from it.
    auto holder = harness::makePeer(region, 0);
    auto bystander = harness::makePeer(region, 1);
    const cme::DomainId lane = holder.createDomain(Domain);

    clearMagic(region.getDomainRecord(lane), coherency);

    const auto joinDomain = [&bystander, lane]
    {
        bystander.joinDomain(lane);
    };
    ctx.check(harness::threw<cme::JoinError>(joinDomain),
              "cleared record magic: joinDomain refuses the domain");
}

// ── a data domain's record, read by the peer that holds it ──────────
// The holder keeps its belief in DRAM, and waitForOwnership re-confirms that belief against the
// record before it hands the caller a guard. A record it cannot read disproves nothing and
// confirms nothing, so the belief is dropped and the acquire waits for a grant instead. Nobody
// grants a domain whose record is unreadable, so the wait runs out.
//
// This is also why deleteDomainLocked's own corrupt-record answer is unreachable from here: the
// lock ahead of it refuses first, and only a record that goes bad between the two would reach it.
void checkHeldRecordCleared(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();
    auto region = harness::createRegion(FormatDomains, FormatPeers);

    auto holder = harness::makePeer(region, 0);
    const cme::DomainId lane = holder.createDomain(Domain);
    {
        // The creator is holder, so this one comes off the resident fast path. Without it the
        // timeout below would not distinguish a dropped belief from a domain never acquired.
        const auto guard = holder.lock(lane);
        ctx.check(static_cast<bool>(guard), "intact record: the creator locks its own domain");
    }

    clearMagic(region.getDomainRecord(lane), coherency);

    const auto lockDomain = [&holder, lane]
    {
        static_cast<void>(holder.lock(lane));
    };
    ctx.check(harness::threw<cme::LockTimeoutError>(lockDomain),
              "cleared record magic: the holder's own acquire times out");
}

// ── a strategy the running build does not implement ─────────────────
// The header names the successor policy every peer in the region must run, and a peer builds its
// own from that name. A value outside the enum is what a region written by a newer build looks
// like, and the factory returning nothing is the only signal the joiner gets.
void checkUnknownStrategy(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();
    auto region = harness::createRegion(FormatDomains, FormatPeers);

    auto* header = region.getHeader();
    auto line = cme::coherency::get(header, coherency);
    line.strategy = UnknownStrategy;
    cme::coherency::set(header, line, coherency);

    const auto joinRegion = [&region, coherency]
    {
        const auto joiner = harness::makePeer(region, 0);
    };
    ctx.check(harness::threw<cme::JoinError>(joinRegion),
              "unknown strategy in the header: the join fails");
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    checkMemberSlotCleared(ctx);
    checkControlRecordCleared(ctx);
    checkJoinRecordCleared(ctx);
    checkHeldRecordCleared(ctx);
    checkUnknownStrategy(ctx);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
