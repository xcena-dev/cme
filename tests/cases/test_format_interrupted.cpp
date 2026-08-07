// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_format_interrupted.cpp -- a region whose RA claim area was never stamped.
//
// region_reject covers a header that never committed, region_corrupt a slot unstamped below it.
// Neither reaches the RA claim array, which no joiner reads, so a region missing it opens and runs
// until the first peer dies. A blank slot is all zeroes, and zero is peer 0's id: without
// RecoveryClaim_t's magic every dead peer reads as already claimed by peer 0.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cme/shared.hpp"
#include "core/layout/geometry.hpp"
#include "core/policy/recovery_authority_layout.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "observe/failpoint.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;
using Claim = cme::RecoveryAuthorityLayout::RecoveryClaim_t;

// The boundary this file's last check arms.
constexpr auto BeforeHeader = cme::failpoint::Boundary::FormatBeforeHeader;

constexpr cme::PeerId MaxPeers = 4;
constexpr cme::DomainId NumDomains = 2;
constexpr cme::DomainId Ceiling = NumDomains + 1;  // control + data
constexpr cme::PeerId Dead = 2;                    // not 0, so a blank slot cannot name it by luck

constexpr std::uint32_t RecoveryDeadlineMs = 20000;

// How long the blank slot has to keep the dead peer where it is. Orders over DeadWindowEffective
// plus a few poll ticks, so a recovery that was going to run would have run inside it.
constexpr std::uint32_t BlankRefusalWindowMs = 3000;

// Roll one claim slot back to the bytes a format that stopped short would leave. The whole 64 B
// line goes through coherency::set, the way format commits it.
void blankClaim(Claim* slot, cme::CoherencyMode coherency)
{
    Claim blank{};
    std::memset(&blank, 0, sizeof(blank));
    cme::coherency::set(slot, blank, coherency);
}

// ── a formatter that died before the header ─────────────────────────
// The blank claim slot above is written by hand. This is the same shape produced the honest way:
// a child killed at FormatBeforeHeader leaves every slot stamped and no header magic, so an opener
// waits out its formatTimeout rather than reading dims it cannot trust.
void checkCrashedFormatter(harness::TestContext& ctx)
{
    harness::formatSession(Ceiling, MaxPeers);  // size the medium first
    const bool died = harness::killChildAt(
        BeforeHeader,
        []
        {
            harness::formatSession(Ceiling, MaxPeers);
        });
    if (!ctx.check(died, "the formatter died at its boundary rather than finishing"))
    {
        return;
    }
    std::printf("  %s: every slot stamped, no header magic\n", cme::failpoint::nameOf(BeforeHeader));

    // A second format finishes what the corpse left, which is the route back a caller has.
    harness::formatSession(Ceiling, MaxPeers);
    auto region = harness::openBoundRegion();
    ctx.check(cme::coherency::get(region.getHeader(), ctx.coherency()).isFormatted(),
              "a region a crashed formatter left is usable once formatted again");
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    auto region = harness::createRegion(Ceiling, MaxPeers);
    harness::seedDataDomains(region, NumDomains);

    std::array<harness::PeerSlot_t, MaxPeers> peers{};
    harness::spawnPeerWorkers(peers, MaxPeers, region, NumDomains);
    harness::sleepMs(1000);  // memberships go Active; ownership spreads
    if (!ctx.check(harness::allPeersJoined(peers, MaxPeers), "every worker joined its domains"))
    {
        harness::joinPeerWorkers(peers, MaxPeers);
        return;
    }

    Claim* slot = cme::RecoveryAuthorityLayout{region}.getClaim(Dead);
    const auto coherency = ctx.coherency();
    blankClaim(slot, coherency);
    const Claim read = cme::coherency::get(slot, coherency);
    ctx.check(!read.isValidMagic(), "the claim slot reads as never formatted");
    ctx.check(read.isAuthoredBy(0), "and its zeroed author would pass for peer 0");

    // The claim the RA would have to win sits in that blank slot, and claim() refuses to stake
    // through one. That refusal is the point: the area's own base comes from the header, so bytes
    // a format never wrote say nothing about whether they are a claim slot at all.
    peers[Dead].frozen.store(true);
    peers[Dead].abandon.store(true);

    const bool leftAlone = harness::holdsFor(
        [&region]
        {
            return !harness::hasMemberStatus(region, Dead, Status::None);
        },
        BlankRefusalWindowMs);
    ctx.check(leftAlone, "a blank claim slot leaves the dead peer for a later format to clear");

    harness::joinPeerWorkers(peers, MaxPeers);

    // Last, because it reformats the region every check above was using.
    if (cme::failpoint::Compiled)
    {
        checkCrashedFormatter(ctx);
    }
    else
    {
        std::printf("no CME_FAILPOINT: only the hand-blanked claim area is checked\n");
    }
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
