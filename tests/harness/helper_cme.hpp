// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_cme.hpp -- the library calls a case makes the same way every time: formatting a region,
// opening it, asking what domains it holds, and dumping the latency trace it collected.
//
// Each of these exists because the API's own default is wrong for a test, or because the call takes
// values the run already knows. Every function has two forms where that helps: one taking values,
// and one taking the TestContext, since the uri, the strategy and the medium's coherency mode all
// come from the run and only the dimensions are the case's own choice.
//
// This is the half of the harness that knows what cme is. helper_util.hpp is the half that does not.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "cme/shared.hpp"
#include "cme/shared_session.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "observe/latency.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"
#include "util/time.hpp"

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

// The form a case actually writes: the uri and the strategy come from the run, and only the dims
// are the case's own choice.
inline cme::Session::FormatOpts_t
formatSession(const TestContext& ctx, std::uint32_t maxDomains, std::uint32_t maxPeers)
{
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
            std::chrono::milliseconds formatTimeout = cme::Session::OpenOpts_t{}.formatTimeout)
{
    cme::Session::OpenOpts_t opts;
    opts.coherency = coherency;
    opts.formatTimeout = formatTimeout;
    return cme::Session::open(uri, opts);
}

// Same, with both the uri and the medium's mode taken from the run. This is the one a case wants:
// getting the mode from anywhere else is how a devdax run ends up asserting CacheCoherent.
[[nodiscard]] inline cme::Session
openSession(const TestContext& ctx,
            std::chrono::milliseconds formatTimeout = cme::Session::OpenOpts_t{}.formatTimeout)
{
    return openSession(ctx.uri(), ctx.coherency(), formatTimeout);
}

// The intra-node tier, same reasoning: SharedSession::open takes the same OpenOpts_t and its bare
// overload carries the same CacheCoherent default.
[[nodiscard]] inline cme::SharedSession
openSharedSession(const TestContext& ctx,
                  std::chrono::milliseconds formatTimeout = cme::Session::OpenOpts_t{}.formatTimeout)
{
    cme::Session::OpenOpts_t opts;
    opts.coherency = ctx.coherency();
    opts.formatTimeout = formatTimeout;
    return cme::SharedSession::open(ctx.uri(), opts);
}

// A region mapped and bound, which is what every accessor past getHeader needs: open() maps the
// bytes, and bindBlocking is what reads the header and computes the section bases. A case wanting
// the unbound state asks ctx.memory().openRegion() for it directly, since that state is then the
// subject rather than a step.
[[nodiscard]] inline cme::Geometry openBoundRegion(const TestContext& ctx,
                                                   std::chrono::milliseconds formatTimeout =
                                                       cme::Session::OpenOpts_t{}.formatTimeout)
{
    auto region = ctx.memory().openRegion();
    region.bindBlocking(formatTimeout, ctx.coherency());
    return region;
}

// The Geometry counterpart of formatSession(ctx, ...), for a case that reaches past Session and
// formats its own region: the strategy comes from the run either way, and only the dims are the
// case's own choice. aggregatorGroups stays a parameter because RequestAgg cases pick it, and 0 is
// the auto the header records for everything else.
[[nodiscard]] inline cme::Geometry createRegion(const TestContext& ctx, std::uint32_t domainCount,
                                                std::uint32_t peerCount,
                                                std::uint32_t aggregatorGroups = 0)
{
    return ctx.memory().createRegion(domainCount, peerCount,
                                     cme::Geometry::FormatOpts_t{ctx.strategy(), aggregatorGroups});
}

// ── reads past the public API ───────────────────────────────────────
// A case that checks what the library wrote reads a slot itself, since no public accessor hands
// out a record. Through coherency::get rather than the field: a slot is one 64 B line, and reading
// a field in place skips the barrier the medium needs, which passes on a coherent host and on no
// other. The mode is a parameter because taking it from anywhere but the run is how a devdax case
// ends up asserting CacheCoherent.

[[nodiscard]] inline cme::Geometry::Member_t
readMemberSlot(const cme::Geometry& region, cme::PeerId peerId, cme::CoherencyMode coherency)
{
    return cme::coherency::get(region.getMemberSlot(peerId), coherency);
}

// The predicate four recovery cases had each written out for themselves: is this peer's slot in
// that state yet. Named separately because it is what waitUntil and holdsFor take.
[[nodiscard]] inline bool hasMemberStatus(const cme::Geometry& region, cme::PeerId peerId,
                                          cme::Geometry::Member_t::Status status,
                                          cme::CoherencyMode coherency)
{
    return readMemberSlot(region, peerId, coherency).hasStatus(status);
}

// The authoritative copy. A case wanting a group shadow asks getDomainRecordShadow directly, since
// the shadow is then the subject rather than a way to read the domain.
[[nodiscard]] inline cme::Geometry::DomainRecord_t
readDomainRecord(const cme::Geometry& region, cme::DomainId domainId, cme::CoherencyMode coherency)
{
    return cme::coherency::get(region.getDomainRecord(domainId), coherency);
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
    const auto startCycles = cme::time::readTimestampCounter();
    const auto startWall = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const auto endCycles = cme::time::readTimestampCounter();
    const auto endWall = std::chrono::steady_clock::now();

    const double windowNs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(endWall - startWall).count());
    const double gigahertz =
        windowNs > 0 ? static_cast<double>(endCycles - startCycles) / windowNs : 0.0;

    cme::trace::writeJsonl(path, gigahertz);
    std::printf("trace-jsonl -> %s\n", path);
}

// Seed @count data domains into slots 1..count via a transient peer 0; genesis ownership
// stays on slot 0 and the real peer 0 re-adopts it. Call once after format.
inline void seedDataDomains(cme::Geometry& region, std::uint32_t count,
                            cme::CoherencyMode coherency)
{
    cme::Peer creator{region, 0, coherency};
    for (std::uint32_t i = 0; i < count; ++i)
    {
        (void)creator.createDomain("lane" + std::to_string(i));
    }
}

}  // namespace harness
