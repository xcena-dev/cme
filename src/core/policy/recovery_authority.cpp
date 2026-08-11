// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// recovery_authority.cpp -- ChainRecoveryAuthorityPolicy implementation.
//
// Oracle C chain instance: each peer's RA target = first alive peer along the
// ring from itself. Direction is immaterial; uniqueness by single walk answer,
// availability by termination at first alive peer.

#include "core/policy/recovery_authority.hpp"

#include <cstdint>
#include <memory>

#include "common/timing.hpp"
#include "config.hpp"
#include "core/policy/liveness.hpp"
#include "core/policy/recovery_authority_layout.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "util/coherency.hpp"
#include "util/endian.hpp"

namespace cme
{

namespace
{

// Named rather than spelled out at all five call sites: the layout is a pointer cast, so
// building one per call costs nothing, and this keeps the reads reading as claim lookups.
[[nodiscard]] RecoveryAuthorityLayout::RecoveryClaim_t*
getClaimSlot(const LocalPeerState& peerState, PeerId deadPeerId) noexcept
{
    return RecoveryAuthorityLayout{peerState}.getClaim(deadPeerId);
}

// The recorded RA no longer owns the claim: retracted, or itself failed. Uses the heartbeat
// FD rather than isAlive -- a crashed RA stays membership-Active until its own RA seizes it.
[[nodiscard]] bool isClaimantGone(const LocalPeerState& peerState, PeerId recordedRA)
{
    if (!peerState.isValidPeer(recordedRA))
    {
        return true;  // retracted (NoPeer) -- nobody owns the resume
    }
    return peerState.getLivenessPolicy()->hasFailed(peerState, recordedRA);
}

// Stuck in Recovering because the RA that staked it died: the slot names a failed or absent
// RA, so nobody is completing it. Selectable for resume -- the FSM is idempotent.
[[nodiscard]] bool isRecoveryStranded(const LocalPeerState& peerState, PeerId deadPeerId)
{
    const auto recoveryClaim = coherency::get(getClaimSlot(peerState, deadPeerId), peerState.getCoherencyMode());
    if (!recoveryClaim.isValidMagic())
    {
        return true;  // no live claim record backing the Recovering status
    }
    return isClaimantGone(peerState, static_cast<PeerId>(recoveryClaim.recoveryAuthority));
}

}  // namespace

PeerId ChainRecoveryAuthorityPolicy::getRecoveryTarget(const LocalPeerState& peerState) const
{
    // While settling/won on a target, stay locked on it regardless of ring order.
    if (settleTarget_ != NoPeer)
    {
        return settleTarget_;
    }

    // First alive peer clockwise = this node's RA target. Crashed-but-uncleaned peers stay
    // Active, so recovery is nearest-first. Bounded rather than the ceiling: admission only
    // grows peerScanBound, so no admitted slot is skipped, and the cache lags PeriodicScanInterval
    // at most -- orders under the silence a target must accumulate.
    const auto& liveness = *peerState.getLivenessPolicy();
    for (const PeerId candidate : peerState.getPeerRing())
    {
        // One slot read serves both tests; isAlive's peerId form would read it again. Hand it
        // to the member cache too -- this walk is the only reader of the dead tail, so without
        // it every getMemberView on those peers misses and re-reads what we just had.
        const auto member = peerState.loadMemberSnapshot(candidate);
        peerState.cacheMemberSnapshot(candidate, member);
        if (liveness.isAlive(member))
        {
            return candidate;
        }
        // Resume a stranded peer. Gated on isRecovering so None slots are never selected.
        if (member.isRecovering() && isRecoveryStranded(peerState, candidate))
        {
            return candidate;
        }
    }
    return NoPeer;
}

bool ChainRecoveryAuthorityPolicy::claim(LocalPeerState& peerState, PeerId deadPeerId)
{
    const PeerId self = peerState.getPeerId();

    if (settleTarget_ == NoPeer)
    {
        // Stake only when the right is up for grabs: unclaimed, held by a dead RA, or already
        // ours. A live claim means yield. The claim line is no-cache, so the lambda sees the
        // current LWW winner without a settle race.
        const bool wrote = coherency::rmwIfTrue(
            getClaimSlot(peerState, deadPeerId), peerState.getCoherencyMode(),
            [&](auto* recoveryClaim)
            {
                if (!recoveryClaim->isValidMagic())
                {
                    return false;  // never stake through an invalid slot
                }
                const auto recordedRA = static_cast<PeerId>(recoveryClaim->recoveryAuthority);
                if (recordedRA != self && !isClaimantGone(peerState, recordedRA))
                {
                    return false;  // a live RA holds the claim -> yield, no write
                }
                recoveryClaim->recoveryAuthority = self;
                return true;
            });
        if (!wrote)
        {
            return false;  // yielded to a live claim or invalid slot; retry next tick
        }
        settleTarget_ = deadPeerId;
        settleWindow_ = timing::Deadline{ClaimSettle};
        return false;  // settling, not confirmed yet
    }

    if (!settleWindow_.expired())
    {
        return false;  // still settling
    }

    // Settle window elapsed: confirm win/loss by re-reading the slot (a later
    // cross-host LWW write may have overwritten us).
    const auto recoveryClaim = coherency::get(getClaimSlot(peerState, deadPeerId), peerState.getCoherencyMode());
    if (recoveryClaim.isValidMagic() && recoveryClaim.isAuthoredBy(self))
    {
        return true;  // won
    }
    settleTarget_ = NoPeer;
    return false;  // lost
}

void ChainRecoveryAuthorityPolicy::resetClaim(LocalPeerState& peerState)
{
    // Retract, then drop the latch so getRecoveryTarget falls back to the ring. Only our own
    // authored word: a later LWW writer may have overwritten us, and their claim stands.
    if (settleTarget_ != NoPeer)
    {
        const PeerId self = peerState.getPeerId();
        coherency::rmwIfTrue(getClaimSlot(peerState, settleTarget_), peerState.getCoherencyMode(), [self](auto* recoveryClaim)
                             {
                                 if (!recoveryClaim->isValidMagic() ||
                                     !recoveryClaim->isAuthoredBy(self))
                                 {
                                     return false;
                                 }
                                 recoveryClaim->retract();
                                 return true;
                             });
    }
    settleTarget_ = NoPeer;
}

void ChainRecoveryAuthorityPolicy::resetClaim(LocalPeerState& peerState, PeerId target)
{
    // Only when actually latched on @target; otherwise it came from the ring walk and we
    // hold no claim to drop.
    if (settleTarget_ == target)
    {
        resetClaim(peerState);
    }
}

void ChainRecoveryAuthorityPolicy::retractClaimsBy(LocalPeerState& peerState, PeerId recoveredPeerId)
{
    // Sweep the whole per-peer claim region, retracting every slot whose author is the
    // just-recovered peer. Indexed by the dead peer; author is the recording RA.
    const std::uint32_t maxPeers = peerState.getMaxPeers();
    for (std::uint32_t deadPeerId = 0; deadPeerId < maxPeers; ++deadPeerId)
    {
        coherency::rmwIfTrue(
            getClaimSlot(peerState, static_cast<PeerId>(deadPeerId)), peerState.getCoherencyMode(),
            [recoveredPeerId](auto* recoveryClaim)
            {
                if (!recoveryClaim->isValidMagic() || !recoveryClaim->isAuthoredBy(recoveredPeerId))
                {
                    return false;
                }
                recoveryClaim->retract();
                return true;
            });
    }
}

std::uint64_t ChainRecoveryAuthorityPolicy::getClaimRegionBytes(std::uint32_t maxPeers) const
{
    return static_cast<std::uint64_t>(maxPeers) * sizeof(RecoveryAuthorityLayout::RecoveryClaim_t);
}

void ChainRecoveryAuthorityPolicy::formatClaimRegion(std::uint8_t* base, std::uint32_t maxPeers) const
{
    // format() publishes the whole region (memset already zeroed it), so no fence here.
    // Stamp magic + the "no RA" sentinel (memset gives recoveryAuthority=0, a valid id).
    const RecoveryAuthorityLayout layout{base};
    for (std::uint32_t peerId = 0; peerId < maxPeers; ++peerId)
    {
        auto* claim = layout.getClaim(peerId);
        claim->magic = RecoveryAuthorityLayout::RecoveryClaim_t::Magic;
        claim->recoveryAuthority = NoPeer;
    }
}

std::unique_ptr<RecoveryAuthorityPolicy> makeRecoveryAuthorityPolicy()
{
    return std::make_unique<ChainRecoveryAuthorityPolicy>();
}

}  // namespace cme
