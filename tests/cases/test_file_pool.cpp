// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_file_pool.cpp -- format puts a file: region in the pool FormatOpts_t::coherency calls for,
// and open joins either pool with the mode the mapping picks.
//
// The case formats one region per pool on the run's mount and asks the filesystem where each
// landed. A mount that maps every file one way has no pool to pick, so there the case skips.

#include <cstdint>
#include <cstdio>
#include <string>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "helper_cme.hpp"
#include "memory/memory.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr std::uint32_t Ceiling = 2;  // control + one data domain
constexpr std::uint32_t MaxPeers = 2;

// True when Session::open joins @uri under the mode the mapping picks.
[[nodiscard]] bool isJoined(const std::string& uri)
{
    try
    {
        const auto session = harness::openSession(uri);
        return true;
    }
    catch (const cme::Error&)
    {
        return false;
    }
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const std::string writeBackPath = ctx.memory().name() + "_wb";
    const std::string writeBackUri = ctx.memory().uriFor("wb");
    (void)std::remove(writeBackPath.c_str());

    auto opts = harness::makeFormatOpts(Ceiling, MaxPeers, ctx.strategy());
    opts.coherency = cme::CoherencyMode::Uncached;
    cme::Session::format(ctx.uri(), opts);
    const auto uncachedPool = cme::FileMemory::readPoolCoherency(ctx.memory().name());
    if (!uncachedPool.has_value())
    {
        harness::TestContext::skip("the mount maps every file one way, so there is no pool to pick");
    }
    ctx.check(uncachedPool == cme::CoherencyMode::Uncached,
              "uc: a region formatted for Uncached sits in the uncached pool");

    opts.coherency = cme::CoherencyMode::Flush;
    cme::Session::format(writeBackUri, opts);
    ctx.check(cme::FileMemory::readPoolCoherency(writeBackPath) == cme::CoherencyMode::Flush,
              "wb: a region formatted for Flush sits in the write-back pool");

    ctx.check(isJoined(ctx.uri()), "uc: open joins with the mode the pool picks");
    ctx.check(isJoined(writeBackUri), "wb: open joins with the mode the pool picks");

    (void)std::remove(writeBackPath.c_str());
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
