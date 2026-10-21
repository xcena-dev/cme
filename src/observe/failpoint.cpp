// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// failpoint.cpp -- the armed-boundary check, compiled only under CME_FAILPOINT.

// Its own header, so the include stays whatever the build says. With CME_FAILPOINT off the rest of
// this file compiles to nothing and include-cleaner then reads the include as unused.
#include "observe/failpoint.hpp"  // NOLINT(misc-include-cleaner)

#if defined(CME_FAILPOINT)

#include <unistd.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "common/ordered.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"

namespace cme::failpoint
{
namespace
{

// One held boundary and its two halves of the handshake. The halves are published, since what the
// other thread has to see with the word is the record write that went with it.
struct HoldSlot_t
{
    ordered::Relaxed<Boundary> boundary{Boundary::None};
    ordered::Published<bool> reached{false};
    ordered::Published<bool> letGo{false};
};

// Relaxed on the boundary and the armed word: the arm happens before the work that reaches the
// boundary, and no other state is being published through them.
ordered::Relaxed<Boundary> g_armedBoundary{Boundary::None};

constexpr std::uint32_t HoldSlots = 4;
constexpr std::uint32_t NoSlot = HoldSlots;
std::array<HoldSlot_t, HoldSlots> g_holds;
ordered::Relaxed<std::uint32_t> g_lastHeld{NoSlot};

// The slot holding @boundary, or NoSlot.
[[nodiscard]] std::uint32_t findHold(Boundary boundary) noexcept
{
    const auto found = std::find_if(g_holds.begin(), g_holds.end(),
                                    [boundary](const HoldSlot_t& held)
                                    {
                                        return held.boundary.load() == boundary;
                                    });
    return static_cast<std::uint32_t>(std::distance(g_holds.begin(), found));
}

[[nodiscard]] bool awaitSlot(std::uint32_t slot, timing::Nanos within) noexcept
{
    if (slot == NoSlot)
    {
        return false;
    }

    // Short: what this waits out is one store by the thread entering the boundary, not any work.
    constexpr timing::Micros LookGap{10};

    return poll::waitUntil(
        [slot]
        {
            return g_holds[slot].reached.load();
        },
        within, LookGap);
}

void releaseSlot(std::uint32_t slot) noexcept
{
    if (slot != NoSlot)
    {
        g_holds[slot].letGo.store(true);
    }
}

}  // namespace

void arm(Boundary boundary) noexcept
{
    g_armedBoundary.store(boundary);
}

void hold(Boundary boundary) noexcept
{
    // A released hold has done its job, so its slot is free for this one.
    for (auto& held : g_holds)
    {
        if (held.boundary.load() != boundary &&
            held.letGo.load())
        {
            held.boundary.store(Boundary::None);
        }
    }

    const auto existing = findHold(boundary);
    const auto slot = (existing != NoSlot) ? existing : findHold(Boundary::None);
    if (slot == NoSlot)
    {
        std::fprintf(stderr, "  [failpoint] no free hold slot for %s\n", readName(boundary));
        std::abort();
    }

    auto& held = g_holds[slot];
    held.reached.reset(false);
    held.letGo.reset(false);
    held.boundary.store(boundary);
    g_lastHeld.store(slot);
}

bool awaitHeld(Boundary boundary, timing::Nanos within) noexcept
{
    return awaitSlot(findHold(boundary), within);
}

void release(Boundary boundary) noexcept
{
    releaseSlot(findHold(boundary));
}

bool awaitHeld(timing::Nanos within) noexcept
{
    return awaitSlot(g_lastHeld.load(), within);
}

void release() noexcept
{
    releaseSlot(g_lastHeld.load());
}

void reach(Boundary boundary) noexcept
{
    if (g_armedBoundary.load() == boundary)
    {
        // Said before the raise, on unbuffered stderr: SIGKILL is uncatchable, so a case that sees
        // no line here knows the boundary went unvisited rather than guessing it from the signal.
        std::fprintf(stderr, "  [failpoint] pid %d reached %s\n",
                     static_cast<int>(::getpid()), readName(boundary));
        std::raise(SIGKILL);  // NOLINT(misc-include-cleaner) POSIX, via <csignal>
        return;
    }

    const auto slot = findHold(boundary);
    if (slot == NoSlot)
    {
        return;
    }

    // The cap is what keeps a case that died without releasing from stopping this thread for good,
    // since the boundary sits in a destructor. LookGap bounds how late the release is noticed.
    constexpr timing::Millis HeldCap{5000};
    constexpr timing::Micros LookGap{10};

    g_holds[slot].reached.store(true);
    const bool freed = poll::waitUntil(
        [slot]
        {
            return g_holds[slot].letGo.load();
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
