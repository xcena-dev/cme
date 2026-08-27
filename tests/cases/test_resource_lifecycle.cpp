// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_resource_lifecycle.cpp -- repeated open/lock/close, counting what does not come back.
//
// Every other case builds its peers once, so a handle leaked per lifecycle shows up only after a
// long run. The counts come from /proc rather than from the library: the question is what the
// process still holds, and a library that leaks is the last thing to report it. Under
// CME_SANITIZE=address the same loop answers the memory half.

#include <dirent.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

#include "cme/shared.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr std::uint32_t MaxDomains = 3;
constexpr std::uint32_t MaxPeers = 2;
constexpr std::uint32_t Cycles = 40;

// One cycle's worth of slack: a freed fd can sit in a cache, and the poll thread of the last peer
// may not have been reaped when the count is read. A leak of one per cycle clears this by 40x.
constexpr std::int64_t Slack = 4;

// How many entries a /proc directory holds, or -1 when it cannot be read.
[[nodiscard]] std::int64_t countProcEntries(const char* what)
{
    const std::string path = "/proc/self/" + std::string{what};
    std::int64_t total = 0;
    if (auto* dir = ::opendir(path.c_str()))
    {
        while (::readdir(dir) != nullptr)
        {
            ++total;
        }
        ::closedir(dir);
        return total;
    }
    return -1;
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    harness::formatSession(MaxDomains, MaxPeers);

    // One cycle first, so the counts below are taken past the one-off allocations: the first open
    // maps the region and starts the first poll thread, and neither is a leak.
    {
        auto session = harness::openSession();
        session.createDomain("warm");
    }

    const std::int64_t fdsBefore = countProcEntries("fd");
    const std::int64_t threadsBefore = countProcEntries("task");
    if (!ctx.check(fdsBefore > 0 && threadsBefore > 0, "/proc/self reports fd and task counts"))
    {
        return;
    }

    for (std::uint32_t cycle = 0; cycle < Cycles; ++cycle)
    {
        auto session = harness::openSession();
        const std::string name = "lane" + std::to_string(cycle % (MaxDomains - 1));
        if (!harness::listsDomain(session, name))
        {
            session.createDomain(name);
        }
        {
            const auto guard = session.lock(name);
        }
        session.deleteDomain(name);
    }

    const std::int64_t fdsAfter = countProcEntries("fd");
    const std::int64_t threadsAfter = countProcEntries("task");

    std::printf("%u cycles: fd %" PRId64 " -> %" PRId64 ", threads %" PRId64 " -> %" PRId64 "\n",
                Cycles, fdsBefore, fdsAfter, threadsBefore, threadsAfter);

    ctx.check(fdsAfter - fdsBefore <= Slack, "no file descriptor was left open per cycle");
    ctx.check(threadsAfter - threadsBefore <= Slack, "no thread was left running per cycle");

    // The region still works after all that, so the teardown freed handles rather than state.
    auto session = harness::openSession();
    session.createDomain("last");
    ctx.check(harness::listsDomain(session, "last"), "the region is still usable afterwards");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
