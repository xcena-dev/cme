// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// failpoint.cpp -- the armed-boundary check, compiled only under CME_FAILPOINT.

// Its own header, so the include stays whatever the build says. With CME_FAILPOINT off the rest of
// this file compiles to nothing and include-cleaner then reads the include as unused.
#include "observe/failpoint.hpp"  // NOLINT(misc-include-cleaner)

#if defined(CME_FAILPOINT)

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>

namespace cme::failpoint
{
namespace
{

// Relaxed both ways: the arm happens before the work that reaches the boundary, and no other state
// is being published through this.
std::atomic<Boundary> armedBoundary{Boundary::None};

}  // namespace

void arm(Boundary boundary) noexcept
{
    armedBoundary.store(boundary, std::memory_order_relaxed);
}

void reach(Boundary boundary) noexcept
{
    if (armedBoundary.load(std::memory_order_relaxed) == boundary)
    {
        // Said before the raise, on unbuffered stderr: SIGKILL is uncatchable, so a case that sees
        // no line here knows the boundary went unvisited rather than guessing it from the signal.
        std::fprintf(stderr, "  [failpoint] pid %d reached %s\n",
                     static_cast<int>(::getpid()), nameOf(boundary));
        std::raise(SIGKILL);
    }
}

}  // namespace cme::failpoint

#endif
