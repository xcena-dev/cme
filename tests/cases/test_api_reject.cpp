// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_api_reject.cpp -- what the API does with arguments and handles it must refuse.
//
// Every other case calls the API the way a caller who read the header would. That leaves the whole
// argument-validation side of Session and Peer unrun: the dimension checks in format, the
// not-joined guard on all six Session entry points, the range and name-length checks in Peer, and
// the two lifecycle results that report "already so" rather than an error.
//
// A moved-from handle is the only way a caller reaches the not-joined guards, since Session and
// Peer cannot be default-constructed. Moving out leaves the source holding a null Impl, which is
// exactly the state those guards describe, and a caller who reuses a moved-from Session gets there
// by accident rather than on purpose. So the guard has to throw instead of dereferencing null.
//
// The exception type is the assertion. threw<T> lets any other exception propagate, so an argument
// refused for the wrong reason fails where it happened rather than passing a laxer check.
//
// Two checks assert that nothing is thrown, and they are the ones that keep the rest honest: a
// second leaveDomain and a joinDomain of a domain already joined are declared idempotent, so they
// must return quietly. Without them an implementation that threw on every unusual argument would
// pass every check above.
//
// Every call under test is a named lambda rather than an argument-position one. clang-format is
// configured Allman with no short lambdas, so a lambda written inside a call gets its braces
// aligned to the argument column, which is where the ragged indentation comes from.

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

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

// Slot 0 is control, slot 1 is the domain the cases below create, slot 2 stays Free so a join of
// an in-range-but-never-created domain has somewhere to point.
constexpr std::uint32_t FormatDomains = 3;
constexpr std::uint32_t FormatPeers = 4;

constexpr const char* Domain = "lane0";

// Exactly MaxNameLen characters, which is one too many: the field has to hold a NUL as well.
const std::string OverlongName(cme::Geometry::DomainRecord_t::MaxNameLen, 'x');

// ── Session::format dimensions ─────────────────────────────────────
// maxDomains counts the control slot, so 1 describes a region with no data domain at all and 0 one
// without even a registry. Both are refused before a backend object is created, which is why this
// runs against the real uri without leaving a region behind.
void checkFormatDims(harness::TestContext& ctx)
{
    const std::string& uri = ctx.uri();
    auto opts = harness::makeFormatOpts(FormatDomains, FormatPeers, ctx.strategy());

    const auto formatRegion = [&uri, &opts]
    {
        cme::Session::format(uri, opts);
    };

    for (const std::uint32_t domainCount : {0u, 1u, cme::MaxDomains + 1})
    {
        opts.maxDomains = domainCount;
        ctx.checkf(harness::threw<cme::FormatError>(formatRegion),
                   "format rejects maxDomains=%u", domainCount);
    }

    opts.maxDomains = FormatDomains;  // back in range, so the loop below fails on peers alone
    for (const std::uint32_t peerCount : {0u, cme::MaxPeers + 1})
    {
        opts.maxPeers = peerCount;
        ctx.checkf(harness::threw<cme::FormatError>(formatRegion),
                   "format rejects maxPeers=%u", peerCount);
    }
}

// ── the Geometry layer under it ────────────────────────────────────
// Session::format screens the counts first, so Geometry::create's own check is only reachable
// through the internal entry point. It is not redundant: Geometry is what a peer opens, and the
// dims it accepts decide the area every later joiner computes.
void checkGeometryDims(harness::TestContext& ctx)
{
    const std::string& uri = ctx.uri();
    const cme::Geometry::FormatOpts_t opts{ctx.strategy()};

    // Above peerCount the extra aggregator groups map to no peer, so they cost region bytes and
    // buy nothing. Both create and format refuse them rather than sizing an area around them.
    const cme::Geometry::FormatOpts_t tooManyGroups{ctx.strategy(), FormatPeers + 1};

    const auto createNoDomains = [&uri, &opts]
    {
        static_cast<void>(cme::Geometry::create(uri, 0, FormatPeers, opts));
    };
    const auto createNoPeers = [&uri, &opts]
    {
        static_cast<void>(cme::Geometry::create(uri, FormatDomains, 0, opts));
    };
    const auto createTooManyGroups = [&uri, &tooManyGroups]
    {
        static_cast<void>(cme::Geometry::create(uri, FormatDomains, FormatPeers, tooManyGroups));
    };

    ctx.check(harness::threw<cme::FormatError>(createNoDomains),
              "Geometry::create rejects domainCount=0");
    ctx.check(harness::threw<cme::FormatError>(createNoPeers),
              "Geometry::create rejects peerCount=0");
    ctx.check(harness::threw<cme::FormatError>(createTooManyGroups),
              "Geometry::create rejects aggregatorGroups above peerCount");

    // format() is public and takes its own opts, so the group check runs there too. An unbound
    // geometry reaches it first: open() maps the region without reading the header, and every
    // section base is still null until bindBlocking computes the layout.
    harness::formatSession(FormatDomains, FormatPeers);
    auto region = ctx.memory().openRegion();  // mapped, not bound: that is what format has to refuse

    const auto formatUnbound = [&region, &opts]
    {
        region.format(opts);
    };
    const auto formatTooManyGroups = [&region, &tooManyGroups]
    {
        region.format(tooManyGroups);
    };

    ctx.check(harness::threw<cme::FormatError>(formatUnbound),
              "Geometry::format refuses an unbound geometry");

    region.bindBlocking(std::chrono::milliseconds{1000}, ctx.coherency());
    ctx.check(harness::threw<cme::FormatError>(formatTooManyGroups),
              "Geometry::format rejects aggregatorGroups above peerCount");
}

// ── a Session whose Impl was moved out ─────────────────────────────
void checkMovedFromSession(harness::TestContext& ctx)
{
    harness::formatSession(FormatDomains, FormatPeers);

    auto session = harness::openSession();
    session.createDomain(Domain);
    const auto live = std::move(session);  // every call below goes to the husk, not to `live`

    // Reading a moved-from object is the subject here, not an oversight. Two checks name it: the
    // bugprone pattern match and the analyzer's own path-sensitive one.
    // NOLINTBEGIN(bugprone-use-after-move, clang-analyzer-cplusplus.Move)
    const auto lockHusk = [&session]
    {
        static_cast<void>(session.lock(Domain));
    };
    const auto joinHusk = [&session]
    {
        session.joinDomain(Domain);
    };
    const auto leaveHusk = [&session]
    {
        session.leaveDomain(Domain);
    };
    const auto createHusk = [&session]
    {
        session.createDomain("lane9");
    };
    const auto deleteHusk = [&session]
    {
        session.deleteDomain(Domain);
    };

    ctx.check(harness::threw<cme::JoinError>(lockHusk),
              "moved-from session: lock reports not joined");
    ctx.check(harness::threw<cme::JoinError>(joinHusk),
              "moved-from session: joinDomain reports not joined");
    ctx.check(harness::threw<cme::JoinError>(leaveHusk),
              "moved-from session: leaveDomain reports not joined");
    ctx.check(harness::threw<cme::JoinError>(createHusk),
              "moved-from session: createDomain reports not joined");
    ctx.check(harness::threw<cme::JoinError>(deleteHusk),
              "moved-from session: deleteDomain reports not joined");

    // The two that report emptiness instead of throwing. tryLock's nullopt means "not now" to
    // every caller, and a husk is a permanent not-now; getDomainNames has no region to scan.
    ctx.check(!session.tryLock(Domain, std::chrono::milliseconds{1}).has_value(),
              "moved-from session: tryLock yields no guard");
    ctx.check(session.getDomainNames().empty(),
              "moved-from session: getDomainNames yields nothing");
    // NOLINTEND(bugprone-use-after-move, clang-analyzer-cplusplus.Move)
}

// ── Peer: range, name length, and the mandatory control domain ─────
void checkPeerArguments(harness::TestContext& ctx)
{
    auto region = harness::createRegion(FormatDomains, FormatPeers);

    const auto joinAsCeilingPeer = [&region]
    {
        const auto rejected = harness::makePeer(region, cme::MaxPeers);
    };
    ctx.check(harness::threw<cme::JoinError>(joinAsCeilingPeer),
              "Peer construction rejects a peer id at the ceiling");

    auto peer = harness::makePeer(region, 0);
    const cme::DomainId lane = peer.createDomain(Domain);
    peer.joinDomain(lane);

    // The region has FormatDomains slots, so that count is the first id outside it. NoDomain is the
    // other shape a caller arrives with: resolveDomainName returns it on a miss, and a caller that
    // forgets to test for it hands it straight back.
    for (const cme::DomainId outside : {cme::DomainId{FormatDomains}, cme::NoDomain})
    {
        const auto joinOutside = [&peer, outside]
        {
            peer.joinDomain(outside);
        };
        const auto leaveOutside = [&peer, outside]
        {
            peer.leaveDomain(outside);
        };
        const auto deleteOutside = [&peer, outside]
        {
            peer.deleteDomain(outside);
        };

        ctx.checkf(harness::threw<cme::JoinError>(joinOutside),
                   "joinDomain rejects domainId %u", outside);
        ctx.checkf(harness::threw<cme::JoinError>(leaveOutside),
                   "leaveDomain rejects domainId %u", outside);
        ctx.checkf(harness::threw<cme::JoinError>(deleteOutside),
                   "deleteDomain rejects domainId %u", outside);
    }

    // Slot 2 is in range and was never created, so its record is Free. The joiner sees that under
    // the control lock and refuses, which is the same answer it gives for a domain deleted while
    // the caller held its id.
    const auto joinFreeSlot = [&peer]
    {
        peer.joinDomain(FormatDomains - 1);
    };
    ctx.check(harness::threw<cme::UnknownDomainError>(joinFreeSlot),
              "joinDomain refuses a slot no create ever claimed");

    // Control underpins create and delete for every peer, so neither leaving nor deleting it is
    // offered. The two throw different types because the caller's mistake differs: leaving control
    // is a participation error, deleting it is not a domain operation at all.
    const auto leaveControl = [&peer]
    {
        peer.leaveDomain(cme::ControlDomainId);
    };
    const auto deleteControl = [&peer]
    {
        peer.deleteDomain(cme::ControlDomainId);
    };
    ctx.check(harness::threw<cme::NotParticipatingError>(leaveControl),
              "leaveDomain refuses the control domain");
    ctx.check(harness::threw<cme::JoinError>(deleteControl),
              "deleteDomain refuses the control domain");

    // A name has to fit a DomainRecord_t with room for the NUL, and an empty one would resolve
    // against every record whose name field was never written.
    const auto createEmptyName = [&peer]
    {
        static_cast<void>(peer.createDomain(""));
    };
    const auto createOverlongName = [&peer]
    {
        static_cast<void>(peer.createDomain(OverlongName));
    };
    ctx.check(harness::threw<cme::FormatError>(createEmptyName),
              "createDomain rejects an empty name");
    ctx.check(harness::threw<cme::FormatError>(createOverlongName),
              "createDomain rejects a name that fills the field");

    // The idempotent pair. A second join is answered from the participation bitmap without taking
    // the control lock, and a leave by a peer that never joined has nothing to retract.
    const auto joinAgain = [&peer, lane]
    {
        peer.joinDomain(lane);
    };
    ctx.check(!harness::threw<cme::Error>(joinAgain),
              "joinDomain of a domain already joined returns quietly");

    auto bystander = harness::makePeer(region, 1);
    const auto leaveUnjoined = [&bystander, lane]
    {
        bystander.leaveDomain(lane);
    };
    ctx.check(!harness::threw<cme::Error>(leaveUnjoined),
              "leaveDomain by a peer that never joined returns quietly");
}

// ── a moved-from Peer ──────────────────────────────────────────────
// Peer's guard tests the successor policy rather than the Impl pointer, because a Peer under
// construction has an Impl before it has a policy. A moved-from Peer has neither.
void checkMovedFromPeer(harness::TestContext& ctx)
{
    auto region = harness::createRegion(FormatDomains, FormatPeers);

    auto peer = harness::makePeer(region, 0);
    const cme::DomainId lane = peer.createDomain(Domain);
    const cme::Peer live{std::move(peer)};

    // NOLINTBEGIN(bugprone-use-after-move, clang-analyzer-cplusplus.Move)
    const auto joinHusk = [&peer, lane]
    {
        peer.joinDomain(lane);
    };
    const auto leaveHusk = [&peer, lane]
    {
        peer.leaveDomain(lane);
    };
    const auto deleteHusk = [&peer, lane]
    {
        peer.deleteDomain(lane);
    };
    const auto createHusk = [&peer]
    {
        static_cast<void>(peer.createDomain("lane9"));
    };
    // NOLINTEND(bugprone-use-after-move, clang-analyzer-cplusplus.Move)

    ctx.check(harness::threw<cme::JoinError>(joinHusk),
              "moved-from peer: joinDomain reports not joined");
    ctx.check(harness::threw<cme::JoinError>(leaveHusk),
              "moved-from peer: leaveDomain reports not joined");
    ctx.check(harness::threw<cme::JoinError>(deleteHusk),
              "moved-from peer: deleteDomain reports not joined");
    ctx.check(harness::threw<cme::JoinError>(createHusk),
              "moved-from peer: createDomain reports not joined");
}

// ── flush with nothing to flush ────────────────────────────────────
// The public flush is what a caller uses on its own payload, so it takes whatever pointer that
// caller has. A null address or a zero length is a no-op rather than a fenced write of nothing.
void checkFlushNoOp(harness::TestContext& ctx)
{
    std::uint64_t target = 0;
    cme::flush(nullptr, sizeof(target), ctx.coherency());
    cme::flush(&target, 0, ctx.coherency());
    ctx.check(target == 0, "flush with no range leaves the target alone");
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    checkFormatDims(ctx);
    checkGeometryDims(ctx);
    checkMovedFromSession(ctx);
    checkPeerArguments(ctx);
    checkMovedFromPeer(ctx);
    checkFlushNoOp(ctx);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
