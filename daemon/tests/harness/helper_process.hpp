// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_process.hpp -- forking the child processes a cross-process case needs, and reaping them.
//
// Its own header because it is the only part of this harness that reaches for POSIX process
// control, and a probe that runs in one process has no reason to compile <sys/wait.h>.
//
// A case here means several processes over one area, which is what the domain mutex is for:
// threads of one process would exclude each other through a plain mutex, so only separate
// processes contend the way requesters do.

#pragma once

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace cmed::harness
{

// Fork one child running @body. Zero means fork failed, and the pid comes back so a phased case can
// reap one group first. A child leaves only through _Exit: returning unlinks the parent's area.
template <typename T_Body>
[[nodiscard]] ::pid_t spawnChild(T_Body body)
{
    const ::pid_t child = ::fork();
    if (child < 0)
    {
        std::perror("fork");
        return 0;
    }
    if (child != 0)
    {
        return child;
    }

    int outcome = 0;
    try
    {
        outcome = body();
    }
    catch (const std::exception& failure)
    {
        std::fprintf(stderr, "child: %s\n", failure.what());
        outcome = 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "child: unknown exception\n");
        outcome = 1;
    }
    std::_Exit(outcome);
}

// Wait for @child and say whether it left through _Exit(0). A child reports its findings through
// the shared area, so the status answers only whether it got that far.
[[nodiscard]] inline bool reapedCleanly(::pid_t child)
{
    if (child == 0)
    {
        return false;
    }

    int status = 0;
    return ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace cmed::harness
