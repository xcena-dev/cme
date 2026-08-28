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

#include "common/poll.hpp"
#include "common/timing.hpp"

namespace cme::failpoint
{
namespace
{

// Relaxed both ways: the arm happens before the work that reaches the boundary, and no other state
// is being published through this.
std::atomic<Boundary> g_armedBoundary{Boundary::None};
std::atomic<bool> g_holdRather{false};

// The two halves of the handshake. Release ordering on each store, since what the other thread has
// to see with the word is the record write that went with it.
std::atomic<bool> g_reachedHold{false};
std::atomic<bool> g_letGo{false};

}  // namespace

void arm(Boundary boundary) noexcept
{
    g_holdRather.store(false, std::memory_order_relaxed);
    g_armedBoundary.store(boundary, std::memory_order_relaxed);
}

void hold(Boundary boundary) noexcept
{
    g_reachedHold.store(false, std::memory_order_relaxed);
    g_letGo.store(false, std::memory_order_relaxed);
    g_holdRather.store(true, std::memory_order_relaxed);
    g_armedBoundary.store(boundary, std::memory_order_relaxed);
}

bool awaitHeld(timing::Nanos within) noexcept
{
    // Short: what this waits out is one store by the thread entering the boundary, not any work.
    constexpr timing::Micros LookGap{10};

    return poll::waitUntil(
        []
        {
            return g_reachedHold.load(std::memory_order_acquire);
        },
        within, LookGap);
}

void release() noexcept
{
    g_letGo.store(true, std::memory_order_release);
}

void reach(Boundary boundary) noexcept
{
    if (g_armedBoundary.load(std::memory_order_relaxed) != boundary)
    {
        return;
    }

    if (!g_holdRather.load(std::memory_order_relaxed))
    {
        // Said before the raise, on unbuffered stderr: SIGKILL is uncatchable, so a case that sees
        // no line here knows the boundary went unvisited rather than guessing it from the signal.
        std::fprintf(stderr, "  [failpoint] pid %d reached %s\n",
                     static_cast<int>(::getpid()), readName(boundary));
        std::raise(SIGKILL);  // NOLINT(misc-include-cleaner) POSIX, via <csignal>
        return;
    }

    // The cap is what keeps a case that died without releasing from stopping this thread for good,
    // since the boundary sits in a destructor. LookGap bounds how late the release is noticed.
    constexpr timing::Millis HeldCap{5000};
    constexpr timing::Micros LookGap{10};

    g_reachedHold.store(true, std::memory_order_release);
    const bool freed = poll::waitUntil(
        []
        {
            return g_letGo.load(std::memory_order_acquire);
        },
        HeldCap, LookGap);
    if (!freed)
    {
        // The cap, not a release. Said out loud because the case that armed this is the one that
        // failed to let go, and its own result will not show that.
        std::fprintf(stderr, "  [failpoint] pid %d held at %s to the cap\n",
                     static_cast<int>(::getpid()), readName(boundary));
    }
}

}  // namespace cme::failpoint

#endif
