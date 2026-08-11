// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor_peterson.hpp -- PetersonPolicy: tournament-is-lock (model A).
//
// Winning the tournament root == holding the domain, so the worker owns the whole lifecycle
// and pollCycle stays empty. No doorbell raise, no holder crown wait. The tree needs a
// power-of-two leaf count (peerCount rounds up) and assumes one contending thread per peer.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/policy/successor_policy.hpp"
#include "core/types.hpp"

namespace cme
{

// Per-peer tournament climb state (nodes view + this peer's leaf->root path).
// Defined in the .cpp; lazily built on the first lock().
struct PetersonState_t;

class PetersonPolicy : public SuccessorPolicy
{
public:
    PetersonPolicy() noexcept;
    ~PetersonPolicy() override;  // out-of-line: unique_ptr<incomplete TournamentLock>

    // ── ops ────────────────────────────────────────────────────────
    // timeout bounds the climb; 0 = single-probe trylock, which is what keeps orphan
    // reclaim's tryLock(Control, 0ns) non-blocking. On timeout the partial climb rolls back.
    OwnershipResult lock(LocalPeerState& peerState, DomainId domainId,
                         const timing::Deadline& deadline) override;
    void unlock(LocalPeerState& peerState, DomainId domainId) override;
    void pollCycle(LocalPeerState& peerState) override;
    void scrubRecoveredPeer(LocalPeerState& peerState, PeerId deadPeerId) noexcept override;
    void bind(LocalPeerState& peerState) noexcept override;

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] Strategy kind() const noexcept override
    {
        return Strategy::Peterson;
    }
    [[nodiscard]] std::uint64_t
    getRegionAreaSize(std::uint32_t domainCount, std::uint32_t peerCount,
                      std::uint32_t aggregatorGroups) const noexcept override;
    void format(std::uint8_t* successorAreaBase, std::uint32_t domainCount,
                std::uint32_t peerCount, std::uint32_t aggregatorGroups,
                CoherencyMode mode) const noexcept override;

private:
    // Per-peer climb state; built by bind() once the region is mapped (needs peerId + dims).
    // The tree shape is domain-independent, so one view serves every domain.
    std::unique_ptr<PetersonState_t> state_;
};

}  // namespace cme
