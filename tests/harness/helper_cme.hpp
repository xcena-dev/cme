// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_cme.hpp -- the library calls a case makes the same way every time: formatting a region,
// opening it, asking what domains it holds, and dumping the latency trace it collected.
//
// Each of these exists because the API's own default is wrong for a test, or because the call takes
// values the run already knows: the uri, the strategy and the medium's coherency mode all come from
// the run, and only the dimensions are the case's own choice.
//
// So the form a case writes says none of them and reaches for currentRun() instead. Where a caller
// has no run -- a bench probe drives the medium without one -- an explicit form taking the values
// sits beside it.
//
// This is the half of the harness that knows what cme is. helper_util.hpp is the half that does not.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cme/shared.hpp"
#include "cme/shared_session.hpp"
#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "observe/latency.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"
#include "util/cpu.hpp"

namespace harness
{

// The three fields every case sets, in one place. Values rather than a TestContext, for the case
// that builds opts it never formats with: api_reject walks dims format is supposed to refuse.
[[nodiscard]] inline cme::Session::FormatOpts_t
makeFormatOpts(std::uint32_t maxDomains, std::uint32_t maxPeers, cme::Strategy strategy)
{
    cme::Session::FormatOpts_t opts;
    opts.maxDomains = maxDomains;
    opts.maxPeers = maxPeers;
    opts.strategy = strategy;
    return opts;
}

// Format @uri with those dims and hand back the opts it used. Returned rather than discarded
// because a case that re-formats mid-run needs the same ones, and building them a second time is
// where the two copies drift.
inline cme::Session::FormatOpts_t
formatSession(const std::string& uri, std::uint32_t maxDomains, std::uint32_t maxPeers,
              cme::Strategy strategy)
{
    const auto opts = makeFormatOpts(maxDomains, maxPeers, strategy);
    cme::Session::format(uri, opts);
    return opts;
}

// The form a case actually writes. The uri and the strategy come from the run, so only the dims
// are said here.
//
// Every implicit form below reaches for currentRun(). A bench has no run, and calls the explicit
// form above instead.
inline cme::Session::FormatOpts_t
formatSession(std::uint32_t maxDomains, std::uint32_t maxPeers)
{
    const TestContext& ctx = currentRun();
    return formatSession(ctx.uri(), maxDomains, maxPeers, ctx.strategy());
}

// Open @uri under the mode the medium needs, which the bare open() overload cannot do:
// OpenOpts_t defaults coherency to CacheCoherent, so a bare open on devdax or an uncacheable
// mount runs a barrier discipline the medium does not have. It still passes on one host, where
// the hardware is coherent whatever the mode says, and that is exactly why it goes unnoticed.
//
// formatTimeout stays at the library default unless a case is waiting out a format that will
// never come, which is the only reason to shorten it.
[[nodiscard]] inline cme::Session
openSession(const std::string& uri, cme::CoherencyMode coherency,
            timing::Millis formatTimeout = cme::Session::OpenOpts_t{}.formatTimeout)
{
    cme::Session::OpenOpts_t opts;
    opts.coherency = coherency;
    opts.formatTimeout = formatTimeout;
    return cme::Session::open(uri, opts);
}

// Same, with both the uri and the medium's mode taken from the run. This is the one a case wants:
// getting the mode from anywhere else is how a devdax run ends up asserting CacheCoherent.
[[nodiscard]] inline cme::Session
openSession(timing::Millis formatTimeout = cme::Session::OpenOpts_t{}.formatTimeout)
{
    const TestContext& ctx = currentRun();
    return openSession(ctx.uri(), ctx.coherency(), formatTimeout);
}

// The intra-node tier, same reasoning: SharedSession::open takes the same OpenOpts_t and its bare
// overload carries the same CacheCoherent default.
[[nodiscard]] inline cme::SharedSession
openSharedSession(timing::Millis formatTimeout = cme::Session::OpenOpts_t{}.formatTimeout)
{
    const TestContext& ctx = currentRun();
    cme::Session::OpenOpts_t opts;
    opts.coherency = ctx.coherency();
    opts.formatTimeout = formatTimeout;
    return cme::SharedSession::open(ctx.uri(), opts);
}

// Whether the acquire succeeded, with the turn given straight back. Templated on both the holder
// and how it names a domain, because the tiers differ on the second: Session and SharedSession take
// a name, Peer takes an id.
//
// The guard dies inside, at the same point the temporary optional would have died anyway, so this
// is only a name for the check. A case that wants to hold what it took keeps the optional itself.
template <typename T_Locker, typename T_Domain>
[[nodiscard]] inline bool canLock(T_Locker& locker, const T_Domain& domain, timing::Nanos budget)
{
    return locker.tryLock(domain, budget).has_value();
}

// Which slot a name resolves to, for a case asking whether the name is live rather than acting on it.
// Peer answers the incarnation alongside the id so that acting on the answer cannot straddle a reuse. A
// case comparing slot numbers has no use for it, and the discard belongs here rather than in each case.
[[nodiscard]] inline cme::DomainId resolvedSlot(const cme::Peer& peer, std::string_view name)
{
    std::uint64_t unusedIncarnation = 0;
    return peer.resolveDomainName(name, unusedIncarnation);
}

// A region mapped and bound, which is what every accessor past getHeader needs: open() maps the
// bytes, and bindBlocking is what reads the header and computes the section bases. A case wanting
// the unbound state asks ctx.memory().openRegion() for it directly, since that state is then the
// subject rather than a step.
[[nodiscard]] inline cme::Geometry
openBoundRegion(timing::Millis formatTimeout = cme::Session::OpenOpts_t{}.formatTimeout)
{
    const TestContext& ctx = currentRun();
    auto region = ctx.memory().openRegion();
    region.bindBlocking(formatTimeout, ctx.coherency());
    return region;
}

// The Geometry counterpart of formatSession, for a case that reaches past Session and formats its
// own region. aggregatorGroups stays a parameter because RequestAgg cases pick it, and 0 is the
// auto the header records for everything else.
[[nodiscard]] inline cme::Geometry
createRegion(std::uint32_t domainCount, std::uint32_t peerCount, std::uint32_t aggregatorGroups = 0)
{
    const TestContext& ctx = currentRun();
    return ctx.memory().createRegion(domainCount, peerCount,
                                     cme::Geometry::FormatOpts_t{ctx.strategy(), aggregatorGroups});
}

// ── reads past the public API ───────────────────────────────────────
// A case that checks what the library wrote reads a slot itself, since no public accessor hands
// out a record. Through coherency::get rather than the field: a slot is one 64 B line, and reading
// a field in place skips the barrier the medium needs, which passes on a coherent host and on no
// other.

// Writing a slot is deliberately absent. A case that writes one is injecting a fault, and the
// injection is that case's subject rather than a way to reach the region.

[[nodiscard]] inline cme::Geometry::Member_t
readMemberSlot(const cme::Geometry& region, cme::PeerId peerId)
{
    return cme::coherency::get(region.getMemberSlot(peerId), currentRun().coherency());
}

// The predicate four recovery cases had each written out for themselves: is this peer's slot in
// that state yet. Named separately because it is what waitUntil and holdsFor take.
[[nodiscard]] inline bool
hasMemberStatus(const cme::Geometry& region, cme::PeerId peerId,
                cme::Geometry::Member_t::Status status)
{
    return readMemberSlot(region, peerId).hasStatus(status);
}

[[nodiscard]] inline bool
participatesIn(const cme::Geometry& region, cme::PeerId peerId, cme::DomainId domainId)
{
    return readMemberSlot(region, peerId).loadParticipatingDomains().has(domainId);
}

// The authoritative copy.
[[nodiscard]] inline cme::Geometry::DomainRecord_t
readDomainRecord(const cme::Geometry& region, cme::DomainId domainId)
{
    return cme::coherency::get(region.getDomainRecord(domainId), currentRun().coherency());
}

// The copy @peerId's group polls, for a case checking whether a publish reached both.
[[nodiscard]] inline cme::Geometry::DomainRecord_t
readDomainRecordShadow(const cme::Geometry& region, cme::DomainId domainId, cme::PeerId peerId)
{
    return cme::coherency::get(region.getDomainRecordShadow(domainId, peerId),
                               currentRun().coherency());
}

// A peer on @region under the run's mode. The mode is the argument a case must not get wrong, and
// a Peer built with the wrong one runs a barrier discipline its medium does not have.
[[nodiscard]] inline cme::Peer makePeer(cme::Geometry& region, cme::PeerId peerId)
{
    return cme::Peer{region, peerId, currentRun().coherency()};
}

// The same, owned, for a case that drops a peer without running its destructor: a crash model
// leaks the Peer so no dtor touches a slot recovery has already finished.
[[nodiscard]] inline std::unique_ptr<cme::Peer> makePeerPtr(cme::Geometry& region,
                                                            cme::PeerId peerId)
{
    return std::make_unique<cme::Peer>(region, peerId, currentRun().coherency());
}

// Peers 0..count-1, each joining data domains 1..domainCount. domainCount = 0 leaves them in the
// control domain alone, which is what a case wants when the region is there to be filled rather
// than worked: admission has no free slot left, and that is the subject.
//
// Owned rather than by value because these outlive the loop that made them and a case reaches back
// into one by index, and because the crash model needs a peer it can drop without a destructor.
[[nodiscard]] inline std::vector<std::unique_ptr<cme::Peer>>
makePeers(cme::Geometry& region, cme::PeerId count, cme::DomainId domainCount = 0)
{
    std::vector<std::unique_ptr<cme::Peer>> peers;
    peers.reserve(count);
    for (cme::PeerId peerId = 0; peerId < count; ++peerId)
    {
        auto peer = makePeerPtr(region, peerId);
        for (cme::DomainId domainId = 1; domainId <= domainCount; ++domainId)
        {
            peer->joinDomain(domainId);
        }
        peers.push_back(std::move(peer));
    }
    return peers;
}

// Whether the region lists @name as an Active data domain. The name is what a caller has, and
// getDomainNames is the only public way to ask, so a create/delete assertion always ends up here.
template <typename T_Session>
[[nodiscard]] bool listsDomain(const T_Session& session, const std::string& name)
{
    const auto names = session.getDomainNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}

// Write the collected span trace to @path, with the TSC frequency the plotter needs to turn cycle
// counts into nanoseconds.
//
// The frequency is measured rather than read from anywhere: there is no portable way to ask, and
// /proc's nominal figure is not what the counter actually ticks at. A 50 ms steady-clock window is
// long enough that the two reads' own cost disappears into it.
//
// writeJsonl compiles to nothing unless the build defines CME_LATENCY, so this is a measurement
// hook rather than a test assertion, and a case reaches it only when asked with --trace-jsonl.
inline void dumpLatencyTrace(const char* path)
{
    const auto startCycles = cme::cpu::readTimestampCounter();
    const timing::Stopwatch window;
    std::this_thread::sleep_for(timing::Millis{50});
    const auto endCycles = cme::cpu::readTimestampCounter();

    const double windowNs = static_cast<double>(window.elapsed().count());
    const double gigahertz =
        windowNs > 0 ? static_cast<double>(endCycles - startCycles) / windowNs : 0.0;

    cme::trace::writeJsonl(path, gigahertz);
    std::printf("trace-jsonl -> %s\n", path);
}

// Seed @count data domains into slots 1..count via a transient peer 0; genesis ownership
// stays on slot 0 and the real peer 0 re-adopts it. Call once after format.
inline void seedDataDomains(cme::Geometry& region, std::uint32_t count)
{
    cme::Peer creator{region, 0, currentRun().coherency()};
    for (std::uint32_t i = 0; i < count; ++i)
    {
        (void)creator.createDomain("lane" + std::to_string(i));
    }
}

}  // namespace harness
