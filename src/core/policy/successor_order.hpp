// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor_order.hpp -- OrderPolicy: ring-based successor.
//
// Holder publishes handoffSeq to ring-next; fair under symmetric load. No
// region-resident layout beyond the protocol sections.

#pragma once

#include <chrono>
#include <cstdint>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/policy/successor_policy.hpp"
#include "core/types.hpp"

namespace cme
{

class OrderPolicy : public SuccessorPolicy
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
        return Strategy::Order;
    }
    [[nodiscard]] std::uint64_t
    getRegionAreaSize(std::uint32_t /*domainCount*/, std::uint32_t /*peerCount*/,
                      std::uint32_t /*aggregatorGroups*/) const noexcept override
    {
        return 0;
    }
};

}  // namespace cme
