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
std::atomic<Boundary> armedBoundary{Boundary::None};
std::atomic<bool> holdRather{false};

// The two halves of the handshake. Release ordering on each store, since what the other thread has
// to see with the word is the record write that went with it.
std::atomic<bool> reachedHold{false};
std::atomic<bool> letGo{false};

}  // namespace

void arm(Boundary boundary) noexcept
{
    holdRather.store(false, std::memory_order_relaxed);
    armedBoundary.store(boundary, std::memory_order_relaxed);
}

void hold(Boundary boundary) noexcept
{
    reachedHold.store(false, std::memory_order_relaxed);
    letGo.store(false, std::memory_order_relaxed);
    holdRather.store(true, std::memory_order_relaxed);
    armedBoundary.store(boundary, std::memory_order_relaxed);
}

bool awaitHeld(timing::Nanos within) noexcept
{
    // Short: what this waits out is one store by the thread entering the boundary, not any work.
    constexpr timing::Micros LookGap{10};

    return poll::waitUntil([]
                           {
                               return reachedHold.load(std::memory_order_acquire);
                           },
                           within, LookGap);
}

void release() noexcept
{
    letGo.store(true, std::memory_order_release);
}

void reach(Boundary boundary) noexcept
{
    if (armedBoundary.load(std::memory_order_relaxed) != boundary)
    {
        return;
    }

    if (!holdRather.load(std::memory_order_relaxed))
    {
        // Said before the raise, on unbuffered stderr: SIGKILL is uncatchable, so a case that sees
        // no line here knows the boundary went unvisited rather than guessing it from the signal.
        std::fprintf(stderr, "  [failpoint] pid %d reached %s\n",
                     static_cast<int>(::getpid()), readName(boundary));
        std::raise(SIGKILL);
        return;
    }

    // The cap is what keeps a case that died without releasing from stopping this thread for good,
    // since the boundary sits in a destructor. LookGap bounds how late the release is noticed.
    constexpr timing::Millis HeldCap{5000};
    constexpr timing::Micros LookGap{10};

    reachedHold.store(true, std::memory_order_release);
    const bool freed = poll::waitUntil([]
                                       {
                                           return letGo.load(std::memory_order_acquire);
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
