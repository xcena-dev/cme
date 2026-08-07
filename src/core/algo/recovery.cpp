// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// recovery.cpp -- SWOT P1 recovery FSM (poll-thread RA monitor).
//
// Per-tick RA FSM: idle-detect -> claim -> takeover -> finalise, mapping to TLA+
// ClaimRecovery / TakeoverOwnership / CompleteRecovery. The 2-phase LWW claim lives in
// RecoveryAuthorityPolicy::claim, spread non-blockingly across ticks; the failure grace
// lives in LivenessPolicy::hasFailed, which is a single stateless comparison of the target's
// last self-stamp against DeadWindowEffective -- the only re-check is claim()'s confirm one
// ClaimSettle later, so a stall need not be continuous for longer than that.

#include "core/algo/recovery.hpp"

#include "config.hpp"
#include "core/algo/ownership_transfer.hpp"
#include "core/layout/geometry.hpp"
#include "core/policy/successor_policy.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/event.hpp"
#include "observe/failpoint.hpp"
#include "observe/observe.hpp"
#include "util/coherency.hpp"

namespace cme::recovery
{

namespace
{

// Clear the dead peer's participation, which stops ORDER-family grants here; REQUEST-family
// grants gate on the demand line and stop at FINISH instead. Idempotent, no concurrent writer.
// Seize the dead peer's slot: clear participation so no holder grants to it again, and stamp
// Recovering, which drops it from isAlive and from re-admission. One rmw, not two -- both fields are
// in the same slot, and a crash between two writes would leave one of them unapplied. RA is the sole
// writer of a dead peer's slot.
void seizeDeadPeerSlot(LocalPeerState& peerState, PeerId deadPeerId)
{
    coherency::rmwIfTrue(
        peerState.getMemberSlot(deadPeerId), peerState.getCoherencyMode(),
        [](auto* member)
        {
            if (!member->isValidMagic())
            {
                return false;
            }
            member->storeParticipatingDomains({});
            member->setStatus(Geometry::Member_t::Status::Recovering);
            return true;
        });
}

// Recovering -> None, then scrub strategy-private state. The RA claim slot is retracted
// separately by resetClaim, since the RA policy owns it.
void finishRecoveryCleanup(LocalPeerState& peerState, PeerId deadPeerId)
{
    CME_FAILPOINT_REACH(failpoint::Boundary::RecoveryBeforeScrub);

    // Strategy-private state the generic record takeover misses: demand line, tournament
    // interest, aggregator duty. RA is the single writer here.
    if (auto* successor = peerState.getSuccessorPolicy())
    {
        successor->scrubRecoveredPeer(peerState, deadPeerId);
    }

    // Arm one sweep unconditionally: reclaimOrphansLocked re-checks each domain under the
    // control lock, so arming costs nothing when there is no orphan.
    peerState.requestOrphanSweep();

    CME_FAILPOINT_REACH(failpoint::Boundary::RecoveryBeforeFinish);

    // None goes last, after every scrub above: getRecoveryTarget skips a None slot, so an RA dying
    // before those writes would leave the tournament interest pinning a live climber with nobody left
    // to clear it. Recovering keeps the target resumable by the next RA.
    coherency::rmwIfTrue(
        peerState.getMemberSlot(deadPeerId), peerState.getCoherencyMode(),
        [](auto* member)
        {
            if (!member->isValidMagic())
            {
                return false;
            }
            // Ends monitoring and frees the slot for re-admission. A resuming slow peer is
            // NOT cut off here -- zombie cutoff is the platform's FAM revoke, not ours.
            member->setStatus(Geometry::Member_t::Status::None);
            return true;
        });
}

// TLA+ TakeoverOwnership: seize the dead peer's domains. Returns true while the pass still finds
// work, which extends the FINISH span. The truth record's holder is the sole authority
// (waitForOwnership settles on it), so a shadow ahead of it is only an incomplete handoff.
bool takeoverDomainsHeldByDeadPeer(LocalPeerState& peerState, PeerId deadPeerId)
{
    bool touched = false;
    for (const DomainId domainId : peerState.getDomainIdRange())
    {
        auto record = coherency::get(peerState.getDomainRecord(domainId), peerState.getCoherencyMode());
        if (record.getHolder() == deadPeerId)
        {
            // Jump clear of an in-flight shadow, which can sit an epoch above the truth:
            // reusing that epoch would break EpochImpliesIdentity.
            record.epoch = record.epoch + RecoverySeizeEpochGap;
            ownership_transfer::takeoverOwnership(peerState, domainId, record);
            OBSERVE_EVENT(Event::RecoveryTakeover, peerState, domainId);
            touched = true;
            CME_FAILPOINT_REACH(failpoint::Boundary::TakeoverMidLoop);
        }
    }
    return touched;
}

}  // namespace

// One tick of the FSM, never blocking: detect (ring target + hasFailed) -> claim (spread
// across ticks) -> takeover (RecoveryCycles ticks, outlasting the visibility bound) -> finish.
void serviceRecovery(LocalPeerState& peerState)
{
    auto& recoveryAuthority = *peerState.getRecoveryAuthorityPolicy();
    auto& liveness = *peerState.getLivenessPolicy();

    const PeerId targetPeerId = recoveryAuthority.getRecoveryTarget(peerState);
    // NoPeer (UINT32_MAX) auto-excluded by unsigned compare; self = nothing to monitor.
    if (!peerState.isValidPeer(targetPeerId) || targetPeerId == peerState.getPeerId())
    {
        peerState.resetRecoveryCycles();  // no target: any span we held is stale
        return;
    }
    if (!liveness.hasFailed(peerState, targetPeerId))
    {
        // Target revived while we held a claim on it. Withdraw, or we early-return every
        // tick, wedged on a live peer. No-op when not latched.
        recoveryAuthority.resetClaim(peerState, targetPeerId);
        peerState.resetRecoveryCycles();
        return;  // watched peer alive; nothing to recover
    }

    // Recovery-work t0 on the trace. Fires every claim tick, so the reader takes the first.
    OBSERVE_EVENT(Event::RecoveryClaimStarted, peerState, targetPeerId);

    if (!recoveryAuthority.claim(peerState, targetPeerId))
    {
        return;  // yielding / settling / lost this tick -- nothing else to do
    }

    // Confirmed RA. Arm the takeover span on the first confirmed tick only
    // (isRecoveryArmed peeks the countdown with no side effect).
    CME_FAILPOINT_REACH(failpoint::Boundary::RecoveryAfterClaim);

    if (!peerState.isRecoveryArmed(targetPeerId))
    {
        seizeDeadPeerSlot(peerState, targetPeerId);
        peerState.setRecoveryCycles(targetPeerId);
        OBSERVE_EVENT(Event::RecoveryClaimed, peerState, targetPeerId);
    }

    // Refill the span whenever the pass still had work: FINISH then needs RecoveryCycles
    // consecutive quiet ticks, so the constant bounds how long we wait for a straggler record
    // rather than deciding on its own that none is left. Refilling (not re-arming from scratch)
    // keeps the Recovering stamp and the participation clear untouched -- both write a line the
    // target's own poll thread also owns.
    if (takeoverDomainsHeldByDeadPeer(peerState, targetPeerId))
    {
        peerState.setRecoveryCycles(targetPeerId);
        return;
    }
    if (peerState.isRecoveryOngoing())
    {
        return;  // quiet this tick, but the span has not run out
    }

    // Retract the target's own claims BEFORE freeing its slot: once None it can re-admit
    // with the same id, and a surviving ghost claim would read as a live RA.
    recoveryAuthority.retractClaimsBy(peerState, targetPeerId);
    finishRecoveryCleanup(peerState, targetPeerId);
    recoveryAuthority.resetClaim(peerState);  // retract our claim on target + drop settle latch
    OBSERVE_EVENT(Event::RecoveryCompleted, peerState, targetPeerId);
}

}  // namespace cme::recovery
