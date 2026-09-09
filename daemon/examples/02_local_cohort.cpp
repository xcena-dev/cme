// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// 02_local_cohort.cpp -- several processes on one node behind one peer slot.
//
// This is what cmed is for. Each child opens its own session and asks for the same domain, and the
// daemon settles them locally: the region sees one peer taking the turn once, not one per child.
// Needs a daemon already serving. Pass its socket path as the first argument.

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cmed/errors.hpp"
#include "cmed/guard.hpp"
#include "cmed/session.hpp"

namespace
{

constexpr std::uint32_t Children = 4;
constexpr std::uint32_t RoundsEach = 3;

// One child's whole life: its own session, its own turns. Returns what the process exits with.
std::int32_t runChild(const std::string& socketPath, std::uint32_t ordinal)
{
    try
    {
        cmed::CmedSession session = cmed::CmedSession::connect(socketPath);
        session.joinDomain("inventory");

        for (std::uint32_t round = 0; round < RoundsEach; ++round)
        {
            const cmed::CmedGuard guard = session.lock("inventory");
            std::printf("child %u holds the turn, round %u\n", ordinal, round);
            std::fflush(stdout);
        }
    }
    catch (const cmed::CmedError& failure)
    {
        std::printf("child %u: %s\n", ordinal, failure.what());
        return 1;
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string socketPath = (argc > 1) ? argv[1] : "/run/cmed/cmed.sock";

    // The parent creates the domain, so a child never races another child to create it.
    try
    {
        cmed::CmedSession session = cmed::CmedSession::connect(socketPath);
        try
        {
            session.createDomain("inventory");
        }
        catch (const cmed::CmedControlRefusedError&)
        {
            session.joinDomain("inventory");
        }

        std::vector<::pid_t> children;
        children.reserve(Children);
        for (std::uint32_t ordinal = 0; ordinal < Children; ++ordinal)
        {
            const ::pid_t forked = ::fork();
            if (forked == 0)
            {
                // The parent's session belongs to the parent. A child that used it would give back a
                // turn its own exit never took.
                ::_exit(runChild(socketPath, ordinal));
            }
            if (forked > 0)
            {
                children.push_back(forked);
            }
        }

        std::uint32_t failures = 0;
        for (const ::pid_t forked : children)
        {
            int status = 0;
            static_cast<void>(::waitpid(forked, &status, 0));
            failures += (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
        }

        std::printf("%u of %u children finished cleanly\n", Children - failures, Children);
        return failures == 0 ? 0 : 1;
    }
    catch (const cmed::CmedError& failure)
    {
        std::printf("cmed: %s\n", failure.what());
        return 1;
    }
}
