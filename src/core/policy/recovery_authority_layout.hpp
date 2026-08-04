// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// recovery_authority_layout.hpp -- what a recovery-authority area looks like.
//
// Area = RecoveryClaim_t[peerCount], indexed by the peer being recovered rather than by the
// peer doing the recovering. TLA+ recoveryClaim[deadPeer].
//
// Built once from whatever already holds the area -- a Geometry for an observer, a
// LocalPeerState for the policy -- and every answer after that comes from what it worked out
// at construction.
//
// That is also the line between this file and the policy: work out where a claim sits here,
// decide who may write it there. Split out because format() lays the slots down, the policy
// finds them again, and a test that plants a zombie claim has to reach the same bytes.
//
// Internal: not in include/cme. Writing a claim word can resurrect a retracted claim, so the
// public API offers operations rather than storage.

#pragma once

#include <cstdint>

#include "core/layout/geometry.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "util/coherency.hpp"
#include "util/endian.hpp"

namespace cme
{

// Befriended below: the one caller that formats the area, and so the one holding a bare base
// rather than an object it can ask for one.
class ChainRecoveryAuthorityPolicy;

class RecoveryAuthorityLayout
{
public:
    // ── record ─────────────────────────────────────────────────────────
    // One peer's claim slot. Carries only the LWW winner id -- "in progress" is the target's
    // status, not something recorded here.
    struct RecoveryClaim_t
    {
        static constexpr std::uint32_t Magic = 0x434D4552u;  // "CMER"

        endian::Field_t<std::uint32_t> magic;              //  0
        endian::Field_t<std::uint32_t> recoveryAuthority;  //  4: recovering RA; NoPeer = none
        std::uint8_t reserved[56];                         //  8..63: pad to 64 B

        // A slot format() never wrote reads as zeroes, which would otherwise pass for "peer 0
        // holds the claim". The magic tells a formatted slot from a blank one.
        [[nodiscard]] bool isValidMagic() const noexcept
        {
            return static_cast<std::uint32_t>(magic) == Magic;
        }

        [[nodiscard]] bool isAuthoredBy(PeerId peer) const noexcept
        {
            return static_cast<PeerId>(recoveryAuthority) == peer;
        }

        // Drop the winner id; magic persists. Caller wmb after.
        void retract() noexcept
        {
            recoveryAuthority = NoPeer;
        }
    };

    // ── sizing, before an area exists ──────────────────────────────────
    // formatClaimRegion() and getClaimRegionBytes() ask this with the width alone.
    [[nodiscard]] static std::uint64_t getAreaBytes(std::uint32_t peerCount) noexcept
    {
        return static_cast<std::uint64_t>(peerCount) * sizeof(RecoveryClaim_t);
    }

    // ── binding to an area ─────────────────────────────────────────────
    // Both take the area off an object that already holds it, so a caller never names a base
    // of its own. The form that does name one is private.

    // For an observer, which holds the region itself.
    explicit RecoveryAuthorityLayout(const Geometry& region) noexcept
        : RecoveryAuthorityLayout{region.getRecoveryAuthorityAreaBase()}
    {
    }

    // For the policy, whose peer already carries the area it was bound to.
    explicit RecoveryAuthorityLayout(const LocalPeerState& peerState) noexcept
        : RecoveryAuthorityLayout{peerState.getRecoveryAuthorityAreaBase()}
    {
    }

    // ── what it worked out ─────────────────────────────────────────────

    // The claim on @deadPeerId, the peer being recovered.
    [[nodiscard]] RecoveryClaim_t* getClaim(PeerId deadPeerId) const noexcept
    {
        return claims_ + deadPeerId;
    }

private:
    // For formatClaimRegion(), which is handed the base of a region still being laid down.
    // Private because any uint8_t* compiles here, and one pointing at a neighbouring area
    // reads as claims that were never written.
    friend class ChainRecoveryAuthorityPolicy;
    explicit RecoveryAuthorityLayout(std::uint8_t* recoveryAuthorityAreaBase) noexcept
        : claims_{reinterpret_cast<RecoveryClaim_t*>(recoveryAuthorityAreaBase)}
    {
    }

    RecoveryClaim_t* claims_;
};

static_assert(sizeof(RecoveryAuthorityLayout::RecoveryClaim_t) == 64, "one cacheline");
static_assert(IsRegionRecord<RecoveryAuthorityLayout::RecoveryClaim_t>);

}  // namespace cme
