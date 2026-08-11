// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_tool_format.cpp -- cme-format, run the way a deployment runs it.
//
// Session::format is mkfs: it zeroes the region and lays fresh peer slots over it, and two callers
// running it at once are not serialised against each other. So laying a region out cannot be a
// start-up step in anything that runs on every node, and cme-format is the one deliberate act it
// has to be instead. What makes that safe is the refusal, and the refusal is what this case is for.
//
// The tool is exec'd rather than linked, because an operator reads the exit code. A case that
// called format() directly would test the library this tool wraps and not the wrapping.
//
// Every run here that expects a 0 passes --force, so no check depends on the medium starting clean.
// A dax window keeps what the last run left in it and has no cleanup fixture, so a case that opened
// with a plain run would pass on shm and be refused on dax.
//
// What that leaves untested is the plain run on a medium nobody has touched. The daemon probe
// covers it, on an shm name it unlinks first.

#include <string>

#include "common/args.hpp"
#include "helper.hpp"
#include "helper_program.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr const char* FirstDomain = "before";
constexpr const char* SecondDomain = "after";

// Refused because the region already answers. A code of its own, so a deployment script can tell
// "someone is using this" from "the arguments were wrong".
constexpr int Refused = 3;
constexpr int Rejected = 1;
constexpr int NoUri = 2;

[[nodiscard]] std::string toolPath()
{
    return cliargs::argStr("--format-tool", std::string{});
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const std::string tool = toolPath();
    if (!ctx.check(!tool.empty(), "the case was told where cme-format is"))
    {
        return;
    }

    const std::string uri = ctx.uri();

    ctx.check(harness::runProgram({tool, "--uri", uri, "--max-domains", "4", "--max-peers", "4",
                                   "--strategy", "peterson", "--domains", FirstDomain, "--force"}) == 0,
              "cme-format lays out a region and creates the domains it was given");

    {
        auto session = harness::openSession();
        ctx.check(harness::listsDomain(session, FirstDomain), "and that domain is on it afterwards");
    }

    const int refused = harness::runProgram({tool, "--uri", uri});
    ctx.checkf(refused == Refused, "a second run is refused with a code of its own, at %d", refused);

    {
        // The refusal has to be a refusal and not a quiet no-op that formatted anyway. A domain
        // created before it is what says the bytes were left alone.
        auto session = harness::openSession();
        ctx.check(harness::listsDomain(session, FirstDomain), "and the region it refused is untouched");
    }

    ctx.check(harness::runProgram({tool, "--uri", uri, "--max-domains", "4", "--max-peers", "4",
                                   "--domains", SecondDomain, "--force"}) == 0,
              "--force is what overrides that refusal");

    {
        // Proof that --force reformatted rather than skipping the work: the old domain cannot
        // survive a region that was zeroed.
        auto session = harness::openSession();
        ctx.check(harness::listsDomain(session, SecondDomain), "the domain asked for this time is there");
        ctx.check(!harness::listsDomain(session, FirstDomain), "and the one from before it is not");
    }

    const int rejected = harness::runProgram({tool, "--uri", uri, "--strategy", "nonsense", "--force"});
    ctx.checkf(rejected == Rejected, "a strategy that is not one is rejected, at %d", rejected);

    const int missing = harness::runProgram({tool});
    ctx.checkf(missing == NoUri, "and no --uri at all is a usage error, at %d", missing);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
