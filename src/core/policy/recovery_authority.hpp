// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// recovery_authority.hpp -- RecoveryAuthorityPolicy hierarchy (SWOT oracle C).
//
// Mirrors TLA+ recoveryAuthorityOf (DesignateRA). An implementation must be unique per dead
// peer, available whenever any peer is active, and deterministic (docs/spec/).
// Default ChainRecoveryAuthorityPolicy: RA(dead) = first alive peer clockwise after @dead.

#pragma once

#include <cstdint>
#include <memory>

#include "common/timing.hpp"
#include "core/types.hpp"

namespace cme
{

class LocalPeerState;  // fwd

// Owns selection AND the 2-phase LWW claim; uniqueness (I1) comes from claim(), not selection.
// claim() must never block -- settle time is spread across poll-thread ticks.
class RecoveryAuthorityPolicy
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    RecoveryAuthorityPolicy(const RecoveryAuthorityPolicy&) = delete;
    RecoveryAuthorityPolicy(RecoveryAuthorityPolicy&&) = delete;
    virtual ~RecoveryAuthorityPolicy() = default;

    // ── operator= ──────────────────────────────────────────────────
    RecoveryAuthorityPolicy& operator=(const RecoveryAuthorityPolicy&) = delete;
    RecoveryAuthorityPolicy& operator=(RecoveryAuthorityPolicy&&) = delete;

    // ── ops ────────────────────────────────────────────────────────
    // Nearest Active predecessor, so the watcher is by construction that peer's RA. Stays
    // locked on an in-progress settle target regardless of ring order; NoPeer if solo.
    [[nodiscard]] virtual PeerId getRecoveryTarget(const LocalPeerState& peerState) const = 0;

    // One tick of the 2-phase LWW claim for @deadPeerId: stake, settle for cross-host
    // visibility, confirm. True only once the window elapsed with our claim still standing.
    [[nodiscard]] virtual bool claim(LocalPeerState& peerState, PeerId deadPeerId) = 0;

    // After FINISH: retract our own claim word and clear settle state, so the policy stops
    // locking onto the now-freed peer.
    virtual void resetClaim(LocalPeerState& peerState) = 0;

    // Same, but only when currently latched on @target: the target revived before FINISH,
    // so we must not hold a claim against a live peer.
    virtual void resetClaim(LocalPeerState& peerState, PeerId target) = 0;

    // Erase every claim word authored by @recoveredPeerId. A re-admitted peer reuses its id,
    // so a ghost claim would read as a live RA and wedge that peer's recovery forever.
    virtual void retractClaimsBy(LocalPeerState& peerState, PeerId recoveredPeerId) = 0;

    // ── region-resident claim slots (RA-policy-private FAM) ────────
    // Policy-private, not core geometry: a per-peer array indexed by the dead peer. Geometry
    // only allocates the section (getClaimRegionBytes) and inits it (formatClaimRegion).

    // Bytes for the per-peer claim slot array (one 64B slot per peer).
    [[nodiscard]] virtual std::uint64_t getClaimRegionBytes(std::uint32_t maxPeers) const = 0;
    // Zero/magic-init the claim region at format (base already laid out).
    virtual void formatClaimRegion(std::uint8_t* base, std::uint32_t maxPeers) const = 0;

protected:
    RecoveryAuthorityPolicy() = default;
};

// Default impl: RA(dead) = first alive peer clockwise after @dead.
class ChainRecoveryAuthorityPolicy : public RecoveryAuthorityPolicy
{
public:
    // ── ops ────────────────────────────────────────────────────────
    [[nodiscard]] PeerId getRecoveryTarget(const LocalPeerState& peerState) const override;
    [[nodiscard]] bool claim(LocalPeerState& peerState, PeerId deadPeerId) override;
    void resetClaim(LocalPeerState& peerState) override;
    void resetClaim(LocalPeerState& peerState, PeerId target) override;
    void retractClaimsBy(LocalPeerState& peerState, PeerId recoveredPeerId) override;
    [[nodiscard]] std::uint64_t getClaimRegionBytes(std::uint32_t maxPeers) const override;
    void formatClaimRegion(std::uint8_t* base, std::uint32_t maxPeers) const override;

private:
    PeerId settleTarget_{NoPeer};  // dead peer we are settling/won a claim on

    // A zero budget, so it reads as expired until a claim opens a real one. settleTarget_ is what
    // says whether a window is open, and a second field for that would be one more to keep in step.
    timing::Deadline settleWindow_{timing::Nanos::zero()};
};

// Factory: returns ChainRecoveryAuthorityPolicy; future kinds selected here.
[[nodiscard]] std::unique_ptr<RecoveryAuthorityPolicy> makeRecoveryAuthorityPolicy();

// RA claim region bytes, delegated to the policy's own sizing. Builds a throwaway policy --
// format/bind only, never hot. Mirrors getSuccessorAreaSize (successor.hpp).
[[nodiscard]] inline std::uint64_t getRecoveryAuthorityAreaSize(std::uint32_t peerCount) noexcept
{
    const std::unique_ptr<RecoveryAuthorityPolicy> policy = makeRecoveryAuthorityPolicy();
    return (policy != nullptr) ? policy->getClaimRegionBytes(peerCount) : 0;
}

}  // namespace cme
