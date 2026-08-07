// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_process.hpp -- forking the child processes a cross-process case needs, and reaping them.
//
// Its own header because it is the only part of the harness that reaches for POSIX process control,
// and a case that runs in one process has no reason to compile <sys/wait.h>.
//
// A case here means several processes over one region, which is what cme is for: threads share a
// peer slot, so only separate processes contend the way peers do.

#pragma once

#include <sys/wait.h>
#include <unistd.h>

// SIGKILL reaches us through <csignal> on a POSIX libc, but the standard promises only the six C
// signals, so include-cleaner cannot see it there and would send us to the deprecated signal.h.
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace harness
{

// Fork @count children, each running body(index), and return how many were spawned. A short return
// means fork failed; the caller reaps what it got and reports.
//
// A child never returns from here: _Exit(0) runs no destructor and flushes no stream. Both matter.
// A returning child would unmap and unlink the region its parent still needs, and it would flush a
// copy of the parent's stdio buffer, printing every line twice.
//
// The barrier a round needs goes between this call and reapChildren, which is why spawning and
// reaping are separate: the children are all alive and waiting at that point.
template <typename T_Body>
[[nodiscard]] std::uint32_t spawnChildren(std::uint32_t count, T_Body body)
{
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const ::pid_t child = ::fork();
        if (child < 0)
        {
            std::perror("fork");
            return index;
        }
        if (child == 0)
        {
            // A child leaves only through _Exit, never by unwinding: past the fork it holds copies
            // of its parent's sessions, whose poll threads do not exist here, so a destructor
            // joining one raises ESRCH and aborts -- which reapChildSignalled would read as a kill.
            try
            {
                body(index);
            }
            catch (const std::exception& error)
            {
                std::fprintf(stderr, "child %u: %s\n", index, error.what());
                std::_Exit(1);
            }
            catch (...)
            {
                std::fprintf(stderr, "child %u: unknown exception\n", index);
                std::_Exit(1);
            }
            std::_Exit(0);
        }
    }
    return count;
}

// Wait for @count children. Their exit status goes unread: a child reports through the shared
// result buffer, so a status here would only say whether it reached _Exit.
inline void reapChildren(std::uint32_t count)
{
    for (std::uint32_t index = 0; index < count; ++index)
    {
        int status = 0;
        static_cast<void>(::wait(&status));
    }
}

// Wait for one child and report whether the failpoint is what ended it. SIGKILL only, because
// reach() raises exactly that: any other signal means the child died of something else, and
// counting it here would let a boundary nobody reached read as a boundary nothing went wrong at.
[[nodiscard]] inline bool reapChildSignalled()
{
    int status = 0;
    static_cast<void>(::wait(&status));
    return WIFSIGNALED(status) &&
           WTERMSIG(status) == SIGKILL;  // NOLINT(misc-include-cleaner) POSIX, via <csignal>
}

}  // namespace harness
