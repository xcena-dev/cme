// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_create_race.cpp -- concurrent createDomain across processes.
//
// N processes each createDomain a distinct name simultaneously. The control
// domain (an ME lock) serializes the registry, so every create must succeed and
// land in a distinct slot -- no collision, no lost create, no DomainLimit while
// slots remain. Exercises createDomain under real cross-process control-lock
// contention.
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "cme/shared.hpp"
#include "test_context.hpp"

namespace
{

constexpr std::uint32_t Creators = 6;
// Ceiling = control(0) + one data slot per creator.
constexpr std::uint32_t MaxDomains = Creators + 1;

constexpr std::int32_t Unset = -2;
constexpr std::int32_t Failed = -1;

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const std::string& uri = ctx.uri();

    cme::Session::FormatOpts_t fopts;
    fopts.maxDomains = MaxDomains;
    fopts.maxPeers = Creators;
    fopts.strategy = cme::Strategy::Request;
    cme::Session::format(uri, fopts);

    // 0 = created ok, else failure
    const auto results = ctx.scratch<std::int32_t>("results", Creators);
    if (!results)
    {
        ctx.check(false, "shared result buffer mapped");
        return;
    }
    results.fill(Unset);

    for (std::uint32_t i = 0; i < Creators; ++i)
    {
        const pid_t pid = ::fork();
        if (pid < 0)
        {
            std::perror("fork");
            ctx.check(false, "fork failed");
            return;
        }
        if (pid == 0)
        {
            std::int32_t exitCode = Failed;
            try
            {
                auto session = cme::Session::open(uri);
                session.createDomain("dom" + std::to_string(i));
                exitCode = 0;
            }
            catch (...)
            {
                exitCode = Failed;
            }
            results[i] = exitCode;
            std::_Exit(0);
        }
    }

    for (std::uint32_t i = 0; i < Creators; ++i)
    {
        int status = 0;
        ::wait(&status);
    }

    // Every creator succeeded.
    bool allOk = true;
    for (std::uint32_t i = 0; i < Creators; ++i)
    {
        if (results[i] != 0)
        {
            allOk = false;
        }
    }
    ctx.check(allOk, "every concurrent createDomain succeeded (control serialized, no loss)");

    // The region now lists all N distinct names in distinct slots.
    auto session = cme::Session::open(uri);
    const auto domainNames = session.getDomainNames();
    ctx.check(domainNames.size() == Creators, "all N data domains present (distinct slots)");
    bool allNamesFound = true;
    for (std::uint32_t i = 0; i < Creators; ++i)
    {
        const std::string want = "dom" + std::to_string(i);
        bool found = false;
        for (const auto& domainNames : domainNames)
        {
            if (domainNames == want)
            {
                found = true;
            }
        }
        if (!found)
        {
            allNamesFound = false;
        }
    }
    ctx.check(allNamesFound, "each creator's name resolved to a live domain");
}

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, runBody);
}
