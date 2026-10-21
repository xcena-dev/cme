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
// An entry whose comment names a CME_state_machine.tla action sits inside that action, so the spec
// says what happens next. The spec abstracts the others away, so only the implementation answers.
//
// SeizeRecovery has no entry: seizeDeadPeerSlot moves both fields in one rmw, so there is no
// window, and that absence is the design rather than a gap.
enum class Boundary : std::uint32_t
{
    None = 0,  // nothing armed; the value every process starts at

    FormatBeforeHeader,  // slots are down, the header magic is not

    AdmissionBeforeStake,        // the lease read free or stalled, the stake not yet written
    AdmissionBeforeWithdraw,     // a late stake held its hold, its nonce not yet withdrawn
    AdmissionReleaseAfterCheck,  // the line read as carrying our nonce, the clear not yet written
    AdmissionBeforeCommit,       // the lease is held, no member slot reserved
    AdmissionAfterCommit,        // the slot reads Active, the lease is still held
    JoinBeforeBaseline,          // JoinMembership: slot Active, domains not baselined

    CreateBeforeActivate,    // the record is Active, the scan-scope bitmap is not
    DeleteBeforeFree,        // participation retracted, the record not yet Free
    DeleteBeforeDeactivate,  // the record is Free, the bitmap bit is not cleared
    ReclaimMidLoop,          // some of a dead peer's orphans reclaimed, some not

    TransferBeforePublish,  // PublishOwnership: belief dropped, record unwritten
    TransferBeforeTruth,    // the shadow is written, the truth copy is not
    ReconcileBeforeLose,    // the truth read named another holder, the belief not yet dropped
    ScanBeforeRefresh,      // a scan refresh is due, the cached scan scopes not yet re-read

    // Leaving published and the poll thread still up, which is the only stretch where a grant that
    // arrives anyway is still forwarded. LeaveBeforeHandoff is past the poll stop, so it is not this.
    LeaveInDrain,
    LeaveBeforeHandoff,  // BeginLeave: Leaving published, domains still held
    LeaveBeforeNone,     // PublishNone: domains handed off, status not yet None

    RecoveryBeforeStake,   // the claim read free or orphaned, our authority not yet written
    RecoveryAfterClaim,    // the claim is won, the slot is not yet Recovering
    TakeoverMidLoop,       // TakeoverOwnership: some of the dead peer's domains moved
    RecoveryBeforeScrub,   // claim retracted, policy-private state not yet scrubbed
    RecoveryBeforeFinish,  // CompleteRecovery: scrubbed, slot not yet None

    PetersonUnlockBeforePins,  // PetersonPolicy::unlock: the climb lock is held, the pin count not yet read
    PetersonLockRefused,       // PetersonPolicy::lock: the climb lock was refused, the call not yet returned
};

// For a case's own output: a failure that names the boundary is one a reader can re-run.
//
// Outside the CME_FAILPOINT gate, and not for want of trying to hide it. An
// uncalled constexpr function emits nothing, so gating buys no bytes, and it would cost the
// -Wswitch below: with no default case, every build catches an enumerator that gained no name.
[[nodiscard]] constexpr const char* readName(Boundary boundary) noexcept
{
    switch (boundary)
    {
        case Boundary::FormatBeforeHeader:
            return "format.before_header";
        case Boundary::AdmissionBeforeStake:
            return "admission.before_stake";
        case Boundary::AdmissionBeforeWithdraw:
            return "admission.before_withdraw";
        case Boundary::AdmissionReleaseAfterCheck:
            return "admission.release_after_check";
        case Boundary::AdmissionBeforeCommit:
            return "admission.before_commit";
        case Boundary::AdmissionAfterCommit:
            return "admission.after_commit";
        case Boundary::JoinBeforeBaseline:
            return "join.before_baseline";
        case Boundary::CreateBeforeActivate:
            return "create.before_activate";
        case Boundary::DeleteBeforeFree:
            return "delete.before_free";
        case Boundary::DeleteBeforeDeactivate:
            return "delete.before_deactivate";
        case Boundary::ReclaimMidLoop:
            return "reclaim.mid_loop";
        case Boundary::TransferBeforePublish:
            return "transfer.before_publish";
        case Boundary::TransferBeforeTruth:
            return "transfer.before_truth";
        case Boundary::ReconcileBeforeLose:
            return "reconcile.before_lose";
        case Boundary::ScanBeforeRefresh:
            return "scan.before_refresh";
        case Boundary::LeaveInDrain:
            return "leave.in_drain";
        case Boundary::LeaveBeforeHandoff:
            return "leave.before_handoff";
        case Boundary::LeaveBeforeNone:
            return "leave.before_none";
        case Boundary::RecoveryBeforeStake:
            return "recovery.before_stake";
        case Boundary::RecoveryAfterClaim:
            return "recovery.after_claim";
        case Boundary::TakeoverMidLoop:
            return "takeover.mid_loop";
        case Boundary::RecoveryBeforeScrub:
            return "recovery.before_scrub";
        case Boundary::RecoveryBeforeFinish:
            return "recovery.before_finish";
        case Boundary::PetersonUnlockBeforePins:
            return "peterson.unlock_before_pins";
        case Boundary::PetersonLockRefused:
            return "peterson.lock_refused";
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

// Arm @boundary to stop the thread that reaches it until it is released. Several can be held at once,
// and the waiter and holder are threads of one process, so no shared word is involved.
void hold(Boundary boundary) noexcept;

// Whether a thread reached the held @boundary and is waiting there, within @within.
[[nodiscard]] bool awaitHeld(Boundary boundary, timing::Nanos within) noexcept;

// Let the thread held at @boundary continue. Safe before anything reaches the boundary: the word it
// reads is stored either way.
void release(Boundary boundary) noexcept;

// The same two for the boundary most recently passed to hold(), for a case that holds one at a time.
[[nodiscard]] bool awaitHeld(timing::Nanos within) noexcept;
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

[[nodiscard]] inline bool awaitHeld(Boundary, timing::Nanos) noexcept
{
    return false;
}

inline void release(Boundary) noexcept
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
