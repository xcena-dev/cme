// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// liveness.hpp -- LivenessPolicy hierarchy (SWOT Layer-0 oracle B).
//
// Every "is this peer dead?" call funnels here. □ Ownership Uniqueness requires no false
// suspicion of a live owner mid-transfer (7-state counterexample); who acts on a suspicion
// is oracle C. Default: HeartbeatLivenessPolicy -- timestamp witness + bounded timeout.

#pragma once

#include <memory>

#include "core/layout/geometry.hpp"
#include "core/types.hpp"

namespace cme
{

class LocalPeerState;  // fwd

class LivenessPolicy
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    LivenessPolicy(const LivenessPolicy&) = delete;
    LivenessPolicy(LivenessPolicy&&) = delete;
    virtual ~LivenessPolicy() = default;

    // ── operator= ──────────────────────────────────────────────────
    LivenessPolicy& operator=(const LivenessPolicy&) = delete;
    LivenessPolicy& operator=(LivenessPolicy&&) = delete;

    // ── ops ────────────────────────────────────────────────────────
    // True if @peerId's liveness witness is recent enough.
    [[nodiscard]] virtual bool isAlive(const LocalPeerState& peerState,
                                       PeerId peerId) const = 0;

    // Same predicate on a slot the caller already read, so a scan needing other fields of it
    // does not pay a second fabric read.
    [[nodiscard]] virtual bool isAlive(const Geometry::Member_t& member) const = 0;

    // Stateless FD: true if @peerId's last liveness stamp is older than the
    // detector window. One read + local clock; no accumulated observer state.
    [[nodiscard]] virtual bool hasFailed(const LocalPeerState& peerState, PeerId peerId) const = 0;

protected:
    LivenessPolicy() = default;
};

// Default impl: wall-clock timestamp witness + bounded timeout (reliable-FD via
// wmb/rmb visibility and a timeout chosen to bound fabric latency + clock skew).
class HeartbeatLivenessPolicy : public LivenessPolicy
{
public:
    // ── ops ────────────────────────────────────────────────────────
    [[nodiscard]] bool isAlive(const LocalPeerState& peerState, PeerId peerId) const override;
    [[nodiscard]] bool isAlive(const Geometry::Member_t& member) const override;
    [[nodiscard]] bool hasFailed(const LocalPeerState& peerState, PeerId peerId) const override;
};

// Factory: returns HeartbeatLivenessPolicy; future kinds selected here.
[[nodiscard]] std::unique_ptr<LivenessPolicy> makeLivenessPolicy();

}  // namespace cme
