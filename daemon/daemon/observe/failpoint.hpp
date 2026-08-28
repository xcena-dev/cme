// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/observe/failpoint.hpp -- named points where a test build can stop this daemon between two
// writes. The mechanism is libcme's, which is internal to that library, so it is here rather than
// shared. CMED_FAILPOINT off compiles every call to nothing.
//
// The daemon is exec'd rather than forked, so a case arms it with `cmed --arm <name>` on the command
// line. Holding is kept for a boundary reached in a requester's own process, where a case can wait.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "common/timing.hpp"

namespace cmed::failpoint
{

// Every boundary a case can arm, each naming a gap between two writes. An enum rather than a free
// string, so a misspelled arm is refused at startup rather than read as "nothing happened".
enum class Boundary : std::uint32_t
{
    None = 0,  // nothing armed; the value every process starts at

    // The region granted the turn and this daemon has not recorded it, so nothing here knows to give
    // it back. What a survivor sees is a turn held by a peer that is gone.
    AcquireBeforeRecord = 1,

    // The turn is held and the requester has not been told, so the slot still reads LockRequested.
    GrantBeforePublish = 2,

    // The grant is published and nobody has been woken, so a requester asleep on the word stays there
    // until its own deadline rather than until the doorbell.
    GrantBeforeWake = 3,

    // The requester has been told the turn is going and the region still records this peer holding it.
    DropBeforeRelease = 4,

    // A peer's connection is gone and the domains it joined have not been given back, so no other node
    // may delete them.
    DepartBeforeGiveBack = 5,

    // The area descriptor is chosen and the Welcome has not gone out, so a requester waits on a
    // handshake that will never answer.
    WelcomeBeforeAnswer = 6,
};

// For a case's own output, and for the arm that names one. Outside the CMED_FAILPOINT gate: an
// uncalled constexpr function emits nothing, and with no default case every build catches an
// enumerator that gained no name.
[[nodiscard]] constexpr const char* readName(Boundary boundary) noexcept
{
    switch (boundary)
    {
        case Boundary::AcquireBeforeRecord:
            return "acquire.before_record";
        case Boundary::GrantBeforePublish:
            return "grant.before_publish";
        case Boundary::GrantBeforeWake:
            return "grant.before_wake";
        case Boundary::DropBeforeRelease:
            return "drop.before_release";
        case Boundary::DepartBeforeGiveBack:
            return "depart.before_give_back";
        case Boundary::WelcomeBeforeAnswer:
            return "welcome.before_answer";
        case Boundary::None:
            return "none";
    }
    return "unknown";
}

// Compiled is what a case asks before it arms anything: without it the arm is a no-op and neither the
// crash nor the hold it is waiting for ever comes.
#if defined(CMED_FAILPOINT)

inline constexpr bool Compiled = true;

// Arms whatever `--arm <name>` in @arguments named, and answers whether the run may go on. The
// command line is the whole of it: this daemon is exec'd, so a case has nowhere else to arm it from.
// False for a name no boundary carries.
[[nodiscard]] bool arm(const std::vector<std::string_view>& arguments) noexcept;

// Arm @boundary to stop the thread that reaches it until release(), clearing the last hold. Waiter
// and holder are threads of one process, so nothing here crosses a process and no shared word does.
void hold(Boundary boundary) noexcept;

// Whether a thread reached the held boundary and is waiting there, within @within.
[[nodiscard]] bool awaitHeld(timing::Nanos within) noexcept;

// Let the held thread continue. Safe before anything reaches the boundary: the word it reads is
// stored either way.
void release() noexcept;

// Die or wait, whichever @boundary was armed for, and nothing if it was not the armed one.
void reach(Boundary boundary) noexcept;

#else

inline constexpr bool Compiled = false;

// Nothing here reaches a boundary, so a run that named one is refused rather than started: it would
// otherwise pass for want of the death it was waiting for.
[[nodiscard]] inline bool arm(const std::vector<std::string_view>& arguments) noexcept
{
    for (std::uint32_t index = 1; index + 1 < arguments.size(); ++index)
    {
        if (arguments[index] == "--arm")
        {
            return false;
        }
    }
    return true;
}

inline void hold(Boundary) noexcept
{
}

[[nodiscard]] inline bool awaitHeld(timing::Nanos) noexcept
{
    return false;
}

inline void release() noexcept
{
}

// The one the serving paths call, so it is a function here rather than a macro: an enumerator costs
// nothing to evaluate, and a macro would be the only name in this header a caller cannot step into.
inline void reach(Boundary) noexcept
{
}

#endif

}  // namespace cmed::failpoint
