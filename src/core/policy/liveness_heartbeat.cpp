// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// liveness_heartbeat.cpp -- HeartbeatLivenessPolicy implementation.
//
// Timestamp witness plus a bounded timeout: isAlive reads Member_t::Status (γ.2), hasFailed
// is stateless -- last self-stamp older than the dead window (γ.4). Successor designation
// (γ.3) belongs to ChainRecoveryAuthorityPolicy, not here.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "common/timing.hpp"
#include "config.hpp"
#include "core/layout/geometry.hpp"
#include "core/policy/liveness.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"

namespace cme
{

bool HeartbeatLivenessPolicy::isAlive(const LocalPeerState& peerState, PeerId peerId) const
{
    if (!peerState.isValidPeer(peerId))
    {
        return false;
    }
    return isAlive(peerState.loadMemberSnapshot(peerId));
}

bool HeartbeatLivenessPolicy::isAlive(const Geometry::Member_t& member) const
{
    // Leaving counts as alive: a draining peer still stamps and still forwards, and dropping
    // it here would hide it from getRecoveryTarget for the whole drain.
    return member.hasStatus(Geometry::Member_t::Status::Active) ||
           member.hasStatus(Geometry::Member_t::Status::Leaving);
}

bool HeartbeatLivenessPolicy::hasFailed(const LocalPeerState& peerState, PeerId peerId) const
{
    // Stateless: dead if the last self-stamp is older than DeadWindowEffective
    // (nominal + δ + cache staleness). One cached read + local wall clock.
    if (!peerState.isValidPeer(peerId))
    {
        return false;
    }
    const timing::WallStamp lastSeen{peerState.getMemberView(peerId, MemberCacheTTL).lastSeenNanos};

    // No age means the stamp sits at or ahead of our clock: the target's own clock runs ahead, so
    // its witness is fresh rather than stale. A slot that never stamped reads as an enormous age
    // and so as dead, which is what an unstamped slot on a peer we consider valid is.
    const std::optional<timing::Nanos> age = lastSeen.age();
    return age.has_value() && *age > DeadWindowEffective;
}

std::unique_ptr<LivenessPolicy> makeLivenessPolicy()
{
    return std::make_unique<HeartbeatLivenessPolicy>();
}

}  // namespace cme
