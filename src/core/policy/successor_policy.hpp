// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor_policy.hpp -- SuccessorPolicy abstract base (SWOT Layer-0 oracle A).
//
// Each concrete kind owns its policy class and any region-resident layout it needs, in its
// own header; successor.hpp is the wrapper with the factory and per-strategy area sizing.
// The kind is recorded in the region header -- mixing kinds within one region is undefined.

#pragma once

#include <chrono>
#include <cstdint>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/types.hpp"

namespace cme
{

class LocalPeerState;  // fwd

class SuccessorPolicy
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    SuccessorPolicy() = default;
    SuccessorPolicy(const SuccessorPolicy&) = delete;
    SuccessorPolicy(SuccessorPolicy&&) = delete;
    virtual ~SuccessorPolicy() = default;

    // ── operator= ──────────────────────────────────────────────────
    SuccessorPolicy& operator=(const SuccessorPolicy&) = delete;
    SuccessorPolicy& operator=(SuccessorPolicy&&) = delete;

    // ── ops ────────────────────────────────────────────────────────
    // join/leave are strategy-agnostic; SuccessorPolicy owns only the divergent ops.
    // timeout bounds the ownership wait (0 = non-blocking single probe).
    virtual OwnershipResult lock(LocalPeerState& peerState, DomainId domainId,
                                 const timing::Deadline& deadline) = 0;
    virtual void unlock(LocalPeerState& peerState, DomainId domainId) = 0;
    virtual void pollCycle(LocalPeerState& peerState) = 0;

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] virtual Strategy kind() const noexcept = 0;

    // Region-resident metadata beyond the protocol sections, placed at the region tail.
    // Depends only on dims and format params; 0 when the policy needs none.
    [[nodiscard]] virtual std::uint64_t
    getRegionAreaSize(std::uint32_t domainCount, std::uint32_t peerCount,
                      std::uint32_t aggregatorGroups) const noexcept = 0;

    // Format-time hook: initialise this policy's region-resident metadata in the
    // (already-laid-out) tail area. Called once by Geometry::format. Default no-op.
    virtual void format(std::uint8_t* successorAreaBase, std::uint32_t domainCount,
                        std::uint32_t peerCount, std::uint32_t aggregatorGroups,
                        CoherencyMode mode) const noexcept
    {
        (void)successorAreaBase;
        (void)domainCount;
        (void)peerCount;
        (void)mode;
        (void)aggregatorGroups;
    }

    // Build per-peer private state now that the region, peer id and dims are set. Called once
    // by Peer before the poll thread spawns, so a policy needs no lazy-init synchronization --
    // lock/unlock (worker) and scrubRecoveredPeer (poll thread) would otherwise race to
    // construct it. Not called on the format path, where the policy is a stateless helper.
    virtual void bind(LocalPeerState& peerState) noexcept
    {
        (void)peerState;
    }

    // Runtime hook: this peer is leaving; hand off any region role it holds (e.g.
    // aggregator duty) to a live peer. Default no-op.
    virtual void leave(LocalPeerState& peerState) noexcept
    {
        (void)peerState;
    }

    // Called once by the RA after recovering @deadPeerId, to scrub the strategy-private
    // region state the generic DomainRecord takeover does not cover. Default no-op.
    virtual void scrubRecoveredPeer(LocalPeerState& peerState, PeerId deadPeerId) noexcept
    {
        (void)peerState;
        (void)deadPeerId;
    }
};

}  // namespace cme
