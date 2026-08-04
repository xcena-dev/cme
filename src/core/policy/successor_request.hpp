// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor_request.hpp -- RequestPolicy: hand-raise successor.
//
// Requester raises its demand bit, holder grants on release/poll; lower latency than
// Order under bursts. Region-resident layout: a per-peer demand bitmap (the request
// signal), policy-private in the successor area (see request_demand_region.hpp).

#pragma once

#include <chrono>
#include <cstdint>

#include "cme/shared.hpp"
#include "core/policy/successor_policy.hpp"
#include "core/types.hpp"

namespace cme
{

class RequestPolicy : public SuccessorPolicy
{
public:
    // ── ops ────────────────────────────────────────────────────────
    OwnershipResult lock(LocalPeerState& peerState, DomainId domainId,
                         std::chrono::nanoseconds timeout) override;
    void unlock(LocalPeerState& peerState, DomainId domainId) override;
    void pollCycle(LocalPeerState& peerState) override;
    // Leaving: drop this peer's demand line so holders stop granting to it.
    void leave(LocalPeerState& peerState) noexcept override;
    // Recovery: scrub the dead peer's demand line (the generic record takeover misses it).
    void scrubRecoveredPeer(LocalPeerState& peerState, PeerId deadPeerId) noexcept override;

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] Strategy kind() const noexcept override
    {
        return Strategy::Request;
    }
    [[nodiscard]] std::uint64_t
    getRegionAreaSize(std::uint32_t domainCount, std::uint32_t peerCount,
                      std::uint32_t aggregatorGroups) const noexcept override;

private:
    // Poll cycles until the next FAM belief reconcile; see pollCycle. Poll-thread only.
    std::uint32_t reconcileCountdown_{0};
};

}  // namespace cme
