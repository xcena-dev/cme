// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_coherency_modes.cpp -- every barrier regime executed over one medium.
//
// Session::open takes its mode from the mapping: shm runs CacheCoherent, the uc mount Uncached,
// devdax Flush. Left to that, two thirds of the barrier discipline go unexecuted on any one
// machine, and Flush is the only value that emits clflushopt.
//
// So this case builds its peers with the mode named outright. It runs the same small handoff
// three times, once per regime, and is registered on shm alone: flushing a cacheable DRAM line is
// legal and costs a few hundred nanoseconds, which is why one medium can carry all three. A hosted
// runner has no device, and this is what keeps the flush path running there.
//
// What the checks read is that a regime's barrier path executed and the handoff still completed.
// They do not read a barrier's effect: shm is coherent whatever the mode, so a build with every
// fence and flush stripped out passes all three. Telling the regimes apart from their effect needs
// two processes over a medium that is not coherent, which no single host offers.
//
// Peer rather than Session, since only Peer takes a mode.
//
// The scenario is the smallest one that crosses a peer boundary. What differs between the modes
// is the fence and flush around each record write, so any real acquire executes it.

#include <cstdint>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

// Slot 0 is control, slot 1 is the domain handed across.
constexpr std::uint32_t FormatDomains = 2;
constexpr std::uint32_t FormatPeers = 2;

constexpr cme::PeerId HolderId = 0;
constexpr cme::PeerId JoinerId = 1;

constexpr const char* Domain = "lane0";

// One poll cycle carries the grant; the rest is slack for a loaded machine.
constexpr timing::Millis GrantWindow{3'000};

// One handoff under @mode, on a region formatted for this iteration alone. A fresh region per
// mode rather than one shared: a record written under one regime and read under another is a
// different question, and mixing the two here would answer neither.
void checkHandoff(harness::TestContext& ctx, cme::CoherencyMode mode, const char* modeName)
{
    auto region = harness::createRegion(FormatDomains, FormatPeers);
    cme::Peer holder{region, HolderId, mode};
    cme::Peer joiner{region, JoinerId, mode};
    const auto created = holder.createDomain(Domain);

    // The joiner reads a registry the holder published under this mode, so the read side of the
    // regime runs before the acquire does.
    if (!ctx.checkf(harness::resolvedSlot(joiner, Domain) == created.id,
                    "%s: the joiner sees the domain", modeName))
    {
        return;
    }

    joiner.joinDomain(created.id);
    const auto granted = joiner.tryLock(created.id, GrantWindow);
    ctx.checkf(granted.has_value(), "%s: the barrier path runs and the handoff completes",
               modeName);
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    checkHandoff(ctx, cme::CoherencyMode::CacheCoherent, "CacheCoherent");
    checkHandoff(ctx, cme::CoherencyMode::Uncached, "Uncached");
    checkHandoff(ctx, cme::CoherencyMode::Flush, "Flush");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
