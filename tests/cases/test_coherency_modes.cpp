// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_coherency_modes.cpp -- every barrier regime executed over one medium.
//
// CoherencyMode is a runtime Session option rather than a property of the region, so a caller
// may pair any mode with any medium. Each test backend hard-codes exactly one: shm reports
// CacheCoherent, the uc mount Uncached, devdax Flush. Taking the mode from the backend is what
// leaves two thirds of the barrier discipline unexecuted on any one machine, and Flush is the
// only value that emits clflushopt.
//
// So this case names the mode itself instead of asking the backend, which is what the
// value-taking openSession overload is for. It runs the same small handoff three times, once
// per regime, and is registered on shm alone: flushing a cacheable DRAM line is legal and costs
// a few hundred nanoseconds, which is why one medium can carry all three. A hosted runner has
// no device, and this is what keeps the flush path running there.
//
// What the checks read is that a regime's barrier path executed and the handoff still completed.
// They do not read a barrier's effect: shm is coherent whatever the mode, so a build with every
// fence and flush stripped out passes all three. Telling the regimes apart from their effect needs
// two processes over a medium that is not coherent, which no single host offers.
//
// Session rather than Peer, unlike the other record-level cases: nothing here names a peer slot
// or reads a record directly, and the mode reaches the library through the public open option.
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

constexpr const char* Domain = "lane0";

// One poll cycle carries the grant; the rest is slack for a loaded machine.
constexpr timing::Millis GrantWindow{3'000};

// One handoff under @mode, on a region formatted for this iteration alone. A fresh region per
// mode rather than one shared: a record written under one regime and read under another is a
// different question, and mixing the two here would answer neither.
void checkHandoff(harness::TestContext& ctx, cme::CoherencyMode mode, const char* modeName)
{
    harness::formatSession(ctx.uri(), FormatDomains, FormatPeers, ctx.strategy());

    auto holder = harness::openSession(ctx.uri(), mode);
    auto joiner = harness::openSession(ctx.uri(), mode);
    holder.createDomain(Domain);

    // The joiner reads a registry the holder published under this mode, so the read side of the
    // regime runs before the acquire does.
    if (!ctx.checkf(harness::listsDomain(joiner, Domain), "%s: the joiner sees the domain",
                    modeName))
    {
        return;
    }

    joiner.joinDomain(Domain);
    const auto granted = joiner.tryLock(Domain, GrantWindow);
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
