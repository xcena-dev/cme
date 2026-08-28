// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// publish_probe.cpp -- attach against the header words that say an area is usable.
//
// abiVersion is both the ready flag and the compatibility check: zero means nobody finished
// stamping, anything unknown means this build would misread the layout. areaBytes catches a
// build that changed the layout without bumping the version. Nothing here waits, because the
// daemon stamps the header before it binds its socket.

#include <cstdint>
#include <string>

#include "cmed/errors.hpp"
#include "harness/helper.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/area.hpp"
#include "shared/posix/mem_file.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

// A version this build does not know. Reading further would misread the layout rather than fail, so
// attach has to refuse before anything else looks at the area.
void refusesAnUnknownAbiVersion(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("an abi version this build does not know");

    const std::string mismatchLabel = scratch.makeAreaName("mismatch");
    cmed::harness::ProbeArea area{mismatchLabel.c_str()};
    cmed::harness::setAbiVersion(area.shared(), cmed::protocol::AbiVersion + 1);

    bool refused = false;
    bool wrongError = false;
    try
    {
        // The cast is what the call is for: this line is expected to throw, not to return an area.
        static_cast<void>(cmed::harness::attachAgain(area.descriptor()));
    }
    catch (const cmed::CmedAreaInvalidError&)
    {
        refused = true;
    }
    catch (const cmed::CmedError&)
    {
        wrongError = true;
    }

    ctx.check(refused, "attach throws CmedAreaInvalidError");
    ctx.check(!wrongError, "and not some other CmedError");

    // The same area and the same call, one word apart. Without this the check above would pass on any
    // failure the mapping happened to have rather than on the version.
    cmed::harness::setAbiVersion(area.shared(), cmed::protocol::AbiVersion);
    ctx.check(cmed::harness::getMaxDomains(cmed::harness::attachAgain(area.descriptor()).shared()) ==
                  cmed::MaxDomains,
              "the same area maps once the version is put back");
}

// A fresh file is the right size and reads as zero. That has to be told apart from a version this
// build does not know, because the two say different things about what to do next.
void refusesAnAreaNobodyStamped(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("an area nobody stamped");

    const std::string silentLabel = scratch.makeAreaName("silent");
    const auto file = posix::MemFile::create(silentLabel, sizeof(cmed::protocol::SharedArea_t));

    bool refused = false;
    bool wrongError = false;
    try
    {
        static_cast<void>(cmed::harness::attachAgain(file.descriptor()));
    }
    catch (const cmed::CmedAreaNotReadyError&)
    {
        refused = true;
    }
    catch (const cmed::CmedError&)
    {
        wrongError = true;
    }

    ctx.check(refused, "attach throws CmedAreaNotReadyError");
    ctx.check(!wrongError, "and not the error a bad version gets");
}

// One version can still disagree about size, when a build patched the layout without bumping
// the version. Reading a short area past its end is a SIGBUS, not a catchable error.
void refusesAnAreaOfAnotherSize(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("an area laid out to another size");

    const std::string resizedLabel = scratch.makeAreaName("resized");
    const auto file = posix::MemFile::create(resizedLabel, sizeof(cmed::protocol::SharedArea_t));
    auto* const area = static_cast<cmed::protocol::SharedArea_t*>(file.base());

    // Stamped by hand: formatArea writes the true size, and what this case needs is a header that
    // claims a layout this build does not have.
    cmed::harness::setAreaBytes(*area,
                                static_cast<std::uint32_t>(sizeof(cmed::protocol::SharedArea_t)) - cmed::CachelineBytes);
    cmed::harness::setAbiVersion(*area, cmed::protocol::AbiVersion);

    bool refused = false;
    try
    {
        static_cast<void>(cmed::harness::attachAgain(file.descriptor()));
    }
    catch (const cmed::CmedAreaInvalidError&)
    {
        refused = true;
    }

    ctx.check(refused, "attach refuses a header whose size is not this build's");
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"publish-probe"};
    return probe::run("publish probe",
                      [&scratch](probe::Context& ctx)
                      {
                          refusesAnUnknownAbiVersion(ctx, scratch);
                          refusesAnAreaNobodyStamped(ctx, scratch);
                          refusesAnAreaOfAnotherSize(ctx, scratch);
                      });
}
