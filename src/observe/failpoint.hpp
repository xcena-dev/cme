// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// failpoint.hpp -- named points where a test build can stop the process at a write boundary.
//
// A test that waits for the gap between two FAM writes to happen by chance never gets the boundary
// it wanted, so the boundaries are named here and a run arms one. CME_FAILPOINT off compiles every
// call to nothing. A fifth axis on the same rule as the other four, but it does not ride
// OBSERVE_EVENT, since a boundary is a place rather than an event.
//
// Dying asks what a survivor makes of the state left behind. Holding asks what this process does
// with a write that landed while the window was open, which needs the process alive to answer.

#pragma once

#include <cstdint>

#include "common/timing.hpp"

namespace cme::failpoint
{

// Every boundary a case can arm. An enum rather than a name, on the rule tests/harness/README.md
// gives for refusing environment variables: a misspelled arm must not read as "nothing happened".
//
// The two groups decide what a case may conclude. SpecAction windows sit inside one
// CME_state_machine.tla action, so the spec says what happens next. Mechanism windows are in the
// lease, shadows, claim word and format, which the spec abstracts away, so only the implementation
// answers for them. report/research/FAILPOINT_TLA_SYNC.md carries the mapping.
//
// SeizeRecovery has no entry: seizeDeadPeerSlot moves both fields in one rmw, so there is no
// window, and that absence is the design rather than a gap.
enum class Category : std::uint32_t
{
    None = 0,
    SpecAction = 1,  // inside one spec action; the spec says what must happen next
    Mechanism = 2,   // in machinery the spec abstracts away; only the implementation answers
};

// The category is the high byte, so readCategory reads it off the value rather than from a second
// table that would drift from this one.
inline constexpr std::uint32_t CategoryShift = 8U;
inline constexpr std::uint32_t SpecBase =
    static_cast<std::uint32_t>(Category::SpecAction) << CategoryShift;
inline constexpr std::uint32_t MechBase =
    static_cast<std::uint32_t>(Category::Mechanism) << CategoryShift;

enum class Boundary : std::uint32_t
{
    None = 0,  // nothing armed; the value every process starts at

    TransferBeforePublish = SpecBase | 1,  // PublishOwnership: belief dropped, record unwritten
    JoinBeforeBaseline = SpecBase | 2,     // JoinMembership: slot Active, domains not baselined
    LeaveBeforeHandoff = SpecBase | 3,     // BeginLeave: Leaving published, domains still held
    LeaveBeforeNone = SpecBase | 4,        // PublishNone: domains handed off, status not yet None
    TakeoverMidLoop = SpecBase | 5,        // TakeoverOwnership: some of the dead peer's domains moved
    RecoveryBeforeFinish = SpecBase | 6,   // CompleteRecovery: scrubbed, slot not yet None

    FormatBeforeHeader = MechBase | 1,      // slots are down, the header magic is not
    AdmissionBeforeCommit = MechBase | 2,   // the lease is held, no member slot reserved
    AdmissionAfterCommit = MechBase | 3,    // the slot reads Active, the lease is still held
    TransferBeforeTruth = MechBase | 4,     // the shadow is written, the truth copy is not
    RecoveryAfterClaim = MechBase | 5,      // the claim is won, the slot is not yet Recovering
    RecoveryBeforeScrub = MechBase | 6,     // claim retracted, policy-private state not yet scrubbed
    CreateBeforeActivate = MechBase | 7,    // the record is Active, the scan-scope bitmap is not
    DeleteBeforeFree = MechBase | 8,        // participation retracted, the record not yet Free
    DeleteBeforeDeactivate = MechBase | 9,  // the record is Free, the bitmap bit is not cleared
    ReclaimMidLoop = MechBase | 10,         // some of a dead peer's orphans reclaimed, some not

    // Leaving published and the poll thread still up, which is the only stretch where a grant that
    // arrives anyway is still forwarded. LeaveBeforeHandoff is past the poll stop, so it is not this.
    LeaveInDrain = MechBase | 11,
};

[[nodiscard]] constexpr Category readCategory(Boundary boundary) noexcept
{
    return static_cast<Category>(static_cast<std::uint32_t>(boundary) >> CategoryShift);
}

// For a case's own output: a failure that names the boundary is one a reader can re-run.
//
// Outside the CME_FAILPOINT gate with readCategory, and not for want of trying to hide it. An
// uncalled constexpr function emits nothing, so gating buys no bytes, and it would cost the
// -Wswitch below: with no default case, every build catches an enumerator that gained no name.
[[nodiscard]] constexpr const char* readName(Boundary boundary) noexcept
{
    switch (boundary)
    {
        case Boundary::TransferBeforePublish:
            return "transfer.before_publish";
        case Boundary::JoinBeforeBaseline:
            return "join.before_baseline";
        case Boundary::LeaveBeforeHandoff:
            return "leave.before_handoff";
        case Boundary::LeaveBeforeNone:
            return "leave.before_none";
        case Boundary::TakeoverMidLoop:
            return "takeover.mid_loop";
        case Boundary::RecoveryBeforeFinish:
            return "recovery.before_finish";
        case Boundary::FormatBeforeHeader:
            return "format.before_header";
        case Boundary::AdmissionBeforeCommit:
            return "admission.before_commit";
        case Boundary::AdmissionAfterCommit:
            return "admission.after_commit";
        case Boundary::TransferBeforeTruth:
            return "transfer.before_truth";
        case Boundary::RecoveryAfterClaim:
            return "recovery.after_claim";
        case Boundary::RecoveryBeforeScrub:
            return "recovery.before_scrub";
        case Boundary::CreateBeforeActivate:
            return "create.before_activate";
        case Boundary::DeleteBeforeFree:
            return "delete.before_free";
        case Boundary::DeleteBeforeDeactivate:
            return "delete.before_deactivate";
        case Boundary::ReclaimMidLoop:
            return "reclaim.mid_loop";
        case Boundary::LeaveInDrain:
            return "leave.in_drain";
        case Boundary::None:
            return "none";
    }
    return "unknown";
}

// Compiled is what a case asks before it arms anything: without it the arm is a no-op and neither
// the crash nor the hold it is waiting for ever comes.
#if defined(CME_FAILPOINT)

inline constexpr bool Compiled = true;

// Arm @boundary to kill this process. A forked child inherits whatever was armed at fork, so a case
// may arm before the fork or inside the child body.
void arm(Boundary boundary) noexcept;

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

#define CME_FAILPOINT_REACH(boundary) ::cme::failpoint::reach(boundary)

#else

inline constexpr bool Compiled = false;

inline void arm(Boundary) noexcept
{
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

#define CME_FAILPOINT_REACH(boundary) static_cast<void>(0)

#endif

}  // namespace cme::failpoint
