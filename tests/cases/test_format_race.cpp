// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_format_race.cpp -- concurrent format leaves one region, not a torn one.
//
// format takes no lock. It memsets, stamps every slot, fences, then commits the header magic last.
// So a second formatter starting after the first committed is rewriting slots a joiner is already
// entitled to read, and the hazard is a region that reads as formatted while half of it is not.
//
// create_race contends the registry through the control lock. Nothing contends format itself.

#include <cstdint>
#include <cstdio>
#include <string>

#include "cme/shared.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr std::uint32_t Formatters = 6;
constexpr std::uint32_t MaxDomains = 4;
constexpr std::uint32_t MaxPeers = 4;

constexpr std::int32_t Unset = -2;
constexpr std::int32_t Failed = -1;

}  // namespace

void runBody(harness::TestContext& ctx)
{
    harness::formatSession(MaxDomains, MaxPeers);  // size the medium before anyone races on it

    const auto results = ctx.scratch<std::int32_t>("results", Formatters);
    if (!results)
    {
        ctx.check(false, "shared result buffer mapped");
        return;
    }
    results.fill(Unset);

    const std::uint32_t spawned = harness::spawnChildren(
        Formatters,
        [&results](std::uint32_t index)
        {
            std::int32_t exitCode = Failed;
            try
            {
                harness::formatSession(MaxDomains, MaxPeers);
                exitCode = 0;
            }
            catch (...)
            {
                exitCode = Failed;
            }
            results[index] = exitCode;
        });

    harness::reapChildren(spawned);
    if (!ctx.check(spawned == Formatters, "every formatter forked"))
    {
        return;
    }

    bool everyFormatReturned = true;
    for (std::uint32_t index = 0; index < Formatters; ++index)
    {
        everyFormatReturned = everyFormatReturned && results[index] == 0;
    }
    ctx.check(everyFormatReturned, "every concurrent format returned without error");

    // The header is the commit point, so its dims are what a joiner will lay a layout over. A mix
    // of two formatters' values here is the tear this case is looking for.
    auto region = harness::openBoundRegion();
    const auto header = cme::coherency::get(region.getHeader(), ctx.coherency());
    ctx.check(header.isFormatted(), "the region reads as formatted");
    ctx.check(header.numDomains == MaxDomains && header.maxPeers == MaxPeers,
              "the header carries the dims that were asked for");
    ctx.check(header.formatGeneration != 0, "and a format generation to tell incarnations apart");

    // Every slot below the header, since the last formatter to stamp them may not be the one whose
    // header magic committed.
    bool everySlotStamped = true;
    for (cme::PeerId peerId = 0; peerId < MaxPeers; ++peerId)
    {
        everySlotStamped = everySlotStamped && harness::readMemberSlot(region, peerId).isValidMagic();
    }
    for (cme::DomainId domainId = 0; domainId < MaxDomains; ++domainId)
    {
        everySlotStamped =
            everySlotStamped && harness::readDomainRecord(region, domainId).isValidMagic();
    }
    ctx.check(everySlotStamped, "every member slot and domain record below it is stamped");

    // The region is not merely self-consistent, it works.
    auto session = harness::openSession();
    session.createDomain("after");
    ctx.check(harness::listsDomain(session, "after"), "a domain can still be created on it");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
