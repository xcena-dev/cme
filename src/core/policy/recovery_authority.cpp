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
#include <optional>

#include "common/timing.hpp"
#include "config.hpp"
#include "core/algo/stake_claim.hpp"
#include "core/policy/liveness.hpp"
#include "core/policy/recovery_authority_layout.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/failpoint.hpp"
#include "util/endian.hpp"

namespace cme
{

namespace
{

// The recovering RA's id, NoPeer while nobody holds the claim.
using ClaimField = stake::FieldWord<RecoveryAuthorityLayout::RecoveryClaim_t, PeerId,
                                    &RecoveryAuthorityLayout::RecoveryClaim_t::recoveryAuthority>;

// The layout is a pointer cast, so building one per call costs nothing.
[[nodiscard]] ClaimField getClaimField(const LocalPeerState& peerState, PeerId deadPeerId) noexcept
{
    return ClaimField{RecoveryAuthorityLayout{peerState}.getClaim(deadPeerId), peerState.getCoherencyMode(), NoPeer};
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
// An unwritten slot reads as NoPeer, so it counts as stranded too: no claim backs its Recovering.
[[nodiscard]] bool isRecoveryStranded(const LocalPeerState& peerState, PeerId deadPeerId)
{
    return isClaimantGone(peerState, getClaimField(peerState, deadPeerId).read());
}

// One dead peer's claim slot as the claim procedure sees it. The value is this RA's own id, and the
// slot is free when unclaimed, already ours, or held by an RA the heartbeat FD has given up on.
class RecoveryClaimWord
{
public:
    using Value = PeerId;

    // A late stake may have deposed an authority mid-write, so it stays until that write has landed.
    static constexpr auto LateHold = config::recovery::LateStakeHold;
    // The recorded authority's death reopens the claim, never time alone.
    static constexpr std::optional<timing::Millis> StallTimeout{};

    RecoveryClaimWord(const LocalPeerState& peerState, PeerId deadPeerId) noexcept
        : peerState_{peerState},
          field_{getClaimField(peerState, deadPeerId)}
    {
    }

    [[nodiscard]] Value read() const
    {
        return field_.read();
    }

    [[nodiscard]] bool isFree(Value recordedRA) const
    {
        return recordedRA == peerState_.getPeerId() || isClaimantGone(peerState_, recordedRA);
    }

    [[nodiscard]] Value makeOwn() const noexcept
    {
        return peerState_.getPeerId();
    }

    // Judged again on the copy the write is made from. The claim line is no-cache, so that copy is
    // the current LWW winner without a settle race, and a live RA's claim is yielded to.
    [[nodiscard]] bool publish(Value self)
    {
        return field_.publishIfFree(
            self,
            [this](PeerId recordedRA)
            {
                return isFree(recordedRA);
            },
            failpoint::Boundary::RecoveryBeforeStake);
    }

    // Only a claim we authored: a later LWW writer's claim stands.
    void retract(Value self) const noexcept
    {
        field_.retract(self);
    }

private:
    const LocalPeerState& peerState_;
    ClaimField field_;
};

}  // namespace

PeerId ChainRecoveryAuthorityPolicy::getRecoveryTarget(const LocalPeerState& peerState) const
{
    // While settling, won, or holding a late stake on a target, stay locked on it regardless of
    // ring order: claim() runs only for the target returned here.
    if (claimTarget_ != NoPeer)
    {
        return claimTarget_;
    }

    // First alive peer clockwise = this node's RA target. Crashed-but-uncleaned peers stay
    // Active, so recovery is nearest-first. Bounded rather than the ceiling: admission only
    // grows peerScanBound, so no admitted slot is skipped, and the cache lags config::poll_thread::ScanRefreshInterval
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
    // getRecoveryTarget hands back claimTarget_ while staked, so a tick never switches targets
    // mid-stake. Staked or not after the tick decides whether it stays latched.
    RecoveryClaimWord word{peerState, deadPeerId};
    const bool won = stake_.tick(word) == stake::Outcome::Won;
    claimTarget_ = stake_.isStaked() ? deadPeerId : NoPeer;
    return won;
}

bool ChainRecoveryAuthorityPolicy::isClaimHeld(const LocalPeerState& peerState, PeerId deadPeerId) const
{
    return getClaimField(peerState, deadPeerId).isMine(peerState.getPeerId());
}

void ChainRecoveryAuthorityPolicy::resetClaim(LocalPeerState& peerState)
{
    // Retract, then drop the latch so getRecoveryTarget falls back to the ring.
    if (claimTarget_ != NoPeer)
    {
        RecoveryClaimWord word{peerState, claimTarget_};
        stake_.reset(word);
    }
    claimTarget_ = NoPeer;
}

void ChainRecoveryAuthorityPolicy::resetClaim(LocalPeerState& peerState, PeerId target)
{
    // Only when actually latched on @target; otherwise it came from the ring walk and we
    // hold no claim to drop.
    if (claimTarget_ == target)
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
        getClaimField(peerState, static_cast<PeerId>(deadPeerId)).retract(recoveredPeerId);
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
