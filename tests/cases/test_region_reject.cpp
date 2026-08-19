// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_region_reject.cpp -- what a peer does with a header it cannot trust.
//
// Every other case hands the library a region it just formatted, so the whole rejection side of
// Geometry has never run: bindBlocking's three throws and every clearLayout in rebind. The header
// carries dims another host wrote, and geometry.cpp:216 says so in as many words -- untrusted.
// Nothing was checking that the check works.
//
// One injection per invalid header, each asserted twice, because the two readers are supposed to
// disagree about how to fail: Session::open throws so a joiner stops, and Inspector::readHeader
// returns nullopt so a monitor sampling a region mid-format reports nothing instead of dying.
//
// The exception type is the assertion. threw<T> lets any other exception propagate, so a header
// rejected for the wrong reason fails where it happened rather than passing a laxer check.
//
// The injection publishes the whole 64 B header line through coherency::set, which is how format
// itself commits it. Writing one field in place would leave the rest of the line unfenced on a
// noncoherent medium, and then a failure would say more about the write than about the check.
//
// shm only. The mappedSize-below-area case needs a mapping sized to the region it holds, and
// neither a fixed 2 MiB devdax window nor a pre-sized uc file is that. What is being tested reads
// header bytes and computes from them, so it does not vary by medium.
//
// The fourth clearLayout, behind isLayoutAligned, is not reachable from here: the section bases
// are offsets from an mmap result, and mmap already returns page-aligned memory.

#include <cstdint>
#include <exception>
#include <string>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
// timing::Millis is an alias, and include-cleaner credits its use to whatever <chrono> declares
// underneath, so it reads the header that names it as unused.
#include "common/timing.hpp"  // NOLINT(misc-include-cleaner)
#include "core/layout/geometry.hpp"
#include "helper.hpp"
#include "observe/inspector.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

// Dims the region is formatted with before each injection. Small on purpose: the mappedSize case
// below raises the header's dims to the ceiling and needs the real mapping to be smaller.
constexpr std::uint32_t FormatDomains = 2;
constexpr std::uint32_t FormatPeers = 2;

// The ceiling isValidDomainCount and isValidPeerCount accept, from core/types.hpp.
constexpr std::uint32_t CountCeiling = 64;

// A short format timeout, so the unformatted case does not spend the default five seconds
// waiting for a formatter that will never come.
constexpr auto JoinTimeout = timing::Millis{100};

// Re-format, then overwrite the header line with @mutate applied to it. Returns nothing: what
// the injection did is asserted through the two readers, not through this.
template <typename T_Mutate>
void injectHeader(harness::TestContext& ctx, const cme::Session::FormatOpts_t& fmtOpts,
                  T_Mutate&& mutate)
{
    cme::Session::format(ctx.uri(), fmtOpts);

    auto view = ctx.memory().openRegion();
    auto* header = view.getHeader();
    const auto coherency = ctx.coherency();

    auto line = cme::coherency::get(header, coherency);
    mutate(line);
    cme::coherency::set(header, line, coherency);
}

// True when the Inspector refuses to report a header, which is rebind returning false.
bool inspectorRefuses(const std::string& uri, cme::CoherencyMode coherency)
{
    auto inspector = cme::Inspector::open(uri, coherency);
    return !inspector.readHeader().has_value();
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const std::string& uri = ctx.uri();
    const auto coherency = ctx.coherency();

    const auto fmtOpts = harness::makeFormatOpts(FormatDomains, FormatPeers, ctx.strategy());

    const auto openRegion = [&ctx]()
    {
        auto session = harness::openSession(JoinTimeout);
        (void)session.getDomainNames();
    };

    // ── the header never became ready ──────────────────────────────────
    // Magic is format's commit point, so clearing it is exactly what a joiner arriving mid-format
    // sees. It waits out formatTimeout and gives up rather than reading dims it cannot trust.
    injectHeader(ctx, fmtOpts,
                 [](cme::Geometry::Header_t& line)
                 {
                     line.magic = 0;
                 });
    ctx.check(harness::threw<cme::RegionNotFormattedError>(openRegion),
              "cleared magic: open reports not-formatted");
    ctx.check(inspectorRefuses(uri, coherency), "cleared magic: inspector reports no header");

    // A version the running build does not implement fails the same readiness test, so a region
    // written by a newer peer is refused rather than read with this build's field offsets.
    injectHeader(ctx, fmtOpts,
                 [](cme::Geometry::Header_t& line)
                 {
                     line.version = cme::Geometry::Header_t::Version + 1;
                 });
    ctx.check(harness::threw<cme::RegionNotFormattedError>(openRegion),
              "unsupported version: open reports not-formatted");
    ctx.check(inspectorRefuses(uri, coherency),
              "unsupported version: inspector reports no header");

    // ── dims outside the accepted range ────────────────────────────────
    // Zero is the boundary the validator rejects from below; a region with no control domain
    // has nowhere to serialise the registry.
    injectHeader(ctx, fmtOpts,
                 [](cme::Geometry::Header_t& line)
                 {
                     line.numDomains = 0;
                 });
    ctx.check(harness::threw<cme::RegionInvalidError>(openRegion), "numDomains=0: open rejects the region");
    ctx.check(inspectorRefuses(uri, coherency), "numDomains=0: inspector reports no header");

    injectHeader(ctx, fmtOpts,
                 [](cme::Geometry::Header_t& line)
                 {
                     line.maxPeers = CountCeiling + 1;
                 });
    ctx.check(harness::threw<cme::RegionInvalidError>(openRegion), "maxPeers above ceiling: open rejects");
    ctx.check(inspectorRefuses(uri, coherency),
              "maxPeers above ceiling: inspector reports no header");

    // Above peerCount the group count only inflates the computed area, so it is rejected on its
    // own rather than being allowed to surface later as a size complaint.
    injectHeader(ctx, fmtOpts,
                 [](cme::Geometry::Header_t& line)
                 {
                     line.aggregatorGroups = FormatPeers + 1;
                 });
    ctx.check(harness::threw<cme::RegionInvalidError>(openRegion),
              "aggregatorGroups above maxPeers: open rejects");
    ctx.check(inspectorRefuses(uri, coherency),
              "aggregatorGroups above maxPeers: inspector reports no header");

    // ── dims valid, but the area they describe does not fit ────────────
    // Both counts are inside the accepted range, so the validator passes them and the size check
    // is the only thing standing between a joiner and reads past the end of the mapping.
    injectHeader(ctx, fmtOpts,
                 [](cme::Geometry::Header_t& line)
                 {
                     line.numDomains = CountCeiling;
                     line.maxPeers = CountCeiling;
                 });
    ctx.check(harness::threw<cme::RegionInvalidError>(openRegion),
              "area larger than the mapping: open rejects");
    ctx.check(inspectorRefuses(uri, coherency),
              "area larger than the mapping: inspector reports no header");

    // ── the control: an untouched region is still accepted ─────────────
    // Without this, every check above would also pass if open threw unconditionally.
    cme::Session::format(uri, fmtOpts);
    ctx.check(!harness::threw<std::exception>(openRegion), "intact region: open succeeds");
    ctx.check(!inspectorRefuses(uri, coherency), "intact region: inspector reports a header");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
