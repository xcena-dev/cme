// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/observe/failpoint.cpp -- the armed-boundary check under CMED_FAILPOINT, and arm() either way.
//
// arm() is outside the gate so a build with the axis off refuses a name rather than running past it.

// With CMED_FAILPOINT off the gated half compiles to nothing, and include-cleaner then reads this
// as unused even though arm() below is declared in it.
#include "daemon/observe/failpoint.hpp"  // NOLINT(misc-include-cleaner)

#if defined(CMED_FAILPOINT)

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

#include "common/poll.hpp"
#include "common/timing.hpp"

namespace cmed::failpoint
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
        // Said before the raise, on unbuffered stderr: SIGKILL is uncatchable, so a case that sees no
        // line here knows the boundary went unvisited rather than guessing it from the signal.
        std::fprintf(stderr, "  [failpoint] pid %d reached %s\n", static_cast<int>(::getpid()),
                     readName(boundary));
        std::raise(SIGKILL);  // NOLINT(misc-include-cleaner) POSIX, via <csignal>
        return;
    }

    // The cap is what keeps a case that died without releasing from stopping this thread for good.
    // LookGap bounds how late the release is noticed.
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
        std::fprintf(stderr, "  [failpoint] pid %d held at %s to the cap\n", static_cast<int>(::getpid()),
                     readName(boundary));
    }
}

namespace
{

// readName read backwards. Here rather than in the header: the only caller is arm() below, since a
// case names a boundary on a command line and never holds one of these.
[[nodiscard]] constexpr Boundary readBoundary(std::string_view name) noexcept
{
    constexpr Boundary Every[] = {
        Boundary::AcquireBeforeRecord,
        Boundary::GrantBeforePublish,
        Boundary::GrantBeforeWake,
        Boundary::DropBeforeRelease,
        Boundary::DepartBeforeGiveBack,
        Boundary::WelcomeBeforeAnswer,
    };

    for (const Boundary boundary : Every)
    {
        if (name == readName(boundary))
        {
            return boundary;
        }
    }
    return Boundary::None;
}

}  // namespace

bool arm(const std::vector<std::string_view>& arguments) noexcept
{
    for (std::uint32_t index = 1; index + 1 < arguments.size(); ++index)
    {
        if (arguments[index] != "--arm")
        {
            continue;
        }

        const Boundary named = readBoundary(arguments[index + 1]);
        if (named == Boundary::None)
        {
            return false;
        }

        g_holdRather.store(false, std::memory_order_relaxed);
        g_armedBoundary.store(named, std::memory_order_relaxed);
        return true;
    }
    return true;
}

}  // namespace cmed::failpoint

#endif
