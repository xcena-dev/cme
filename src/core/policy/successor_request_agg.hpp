// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor_request_agg.hpp -- RequestAggPolicy: hand-raise + packed aggregation.
//
// Same protocol as RequestPolicy, but the holder's successor scan reads packed
// per-peer request slots instead of an O(N) per-peer Member sweep. What the area looks
// like is successor_request_agg_layout.hpp; this is the policy that maintains it.

#pragma once

#include <cassert>
#include <cstdint>
#include <optional>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/domain_bitmap.hpp"
#include "core/policy/successor_policy.hpp"
#include "core/policy/successor_request_agg_layout.hpp"
#include "core/types.hpp"

namespace cme
{

class RequestAggPolicy : public SuccessorPolicy
{
public:
    // ── ops ────────────────────────────────────────────────────────
    OwnershipResult lock(LocalPeerState& peerState, DomainId domainId,
                         const timing::Deadline& deadline) override;
    void unlock(LocalPeerState& peerState, DomainId domainId) override;
    void pollCycle(LocalPeerState& peerState) override;

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] Strategy kind() const noexcept override
    {
        return Strategy::RequestAgg;
    }
    [[nodiscard]] std::uint64_t
    getRegionAreaSize(std::uint32_t domainCount, std::uint32_t peerCount,
                      std::uint32_t aggregatorGroups) const noexcept override;
    void format(std::uint8_t* successorAreaBase, std::uint32_t domainCount,
                std::uint32_t peerCount, std::uint32_t aggregatorGroups,
                CoherencyMode mode) const noexcept override;
    void bind(LocalPeerState& peerState) noexcept override;
    void leave(LocalPeerState& peerState) noexcept override;
    void scrubRecoveredPeer(LocalPeerState& peerState, PeerId deadPeerId) noexcept override;

private:
    // The one place the "bound before used" invariant is stated. bind() runs before every
    // entry point that reaches the area, so the layout is always there by the time one asks;
    // the assert says so to a reader and to a static analyser, and compiles away under NDEBUG.
    [[nodiscard]] const RequestAggLayout& getLayout() const noexcept
    {
        assert(layout_.has_value());
        // The assert above is the check; clang-tidy does not read it as one. The factory
        // builds this policy before any peer exists, so the type cannot require the layout
        // at construction the way Session::Impl requires its Geometry.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        return *layout_;
    }

    // ── internals needing the bound geometry ───────────────────────
    void refreshAggregatedRequest(LocalPeerState& peerState);
    void updateDemandAndSlice(LocalPeerState& peerState, DomainId domainId,
                              bool requesting) noexcept;
    [[nodiscard]] bool transferHeldDomainsAgg(LocalPeerState& peerState, DomainBitmap heldDomains);

    // ── bind()-time group geometry ─────────────────────────────────
    // All three are fixed once the peer is bound, so they are resolved there rather than per
    // call: rebuilding the layout costs three integer divisions, and lock() would pay them.

    std::uint32_t groupId_{0};         // this peer's group
    std::uint32_t slotLocalIndex_{0};  // this peer's slice within its group's slot

    // Empty until bind(), which is the earliest point the dimensions are known: the factory
    // builds this policy before any peer is attached to it. Optional rather than a zeroed
    // layout so that "not bound yet" stays a state one can test for.
    std::optional<RequestAggLayout> layout_;

    // Poll cycles until the next FAM belief reconcile; see RequestPolicy. Poll-thread only.
    std::uint32_t reconcileCountdown_{0};
};

}  // namespace cme
