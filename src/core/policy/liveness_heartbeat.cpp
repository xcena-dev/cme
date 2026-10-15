// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// liveness_heartbeat.cpp -- HeartbeatLivenessPolicy implementation.
//
// Timestamp witness plus a bounded timeout: isAlive reads Member_t::Status (γ.2), hasFailed
// is stateless -- last self-stamp older than the dead window (γ.4). Successor designation
// (γ.3) belongs to ChainRecoveryAuthorityPolicy, not here.

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

namespace
{

// No age means the stamp is at or ahead of our clock, so the target's clock runs ahead and its witness
// is fresh. A never-stamped slot reads as an enormous age, hence dead, as it should on a valid peer.
[[nodiscard]] bool isPastDeadWindow(std::uint64_t lastSeenNanos)
{
    const std::optional<timing::Nanos> age = timing::WallStamp{lastSeenNanos}.age();
    return age.has_value() && *age > config::liveness::DeadWindowEffective;
}

}  // namespace

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
    // Stateless: dead if the last self-stamp is older than config::liveness::DeadWindowEffective
    // (nominal + δ + cache staleness). One cached read + local wall clock.
    if (!peerState.isValidPeer(peerId))
    {
        return false;
    }
    return isPastDeadWindow(peerState.getMemberView(peerId, config::membership::CacheTTL).lastSeenNanos);
}

bool HeartbeatLivenessPolicy::hasFailed(const Geometry::Member_t& member) const
{
    return isPastDeadWindow(member.lastSeenNanos);
}

std::unique_ptr<LivenessPolicy> makeLivenessPolicy()
{
    return std::make_unique<HeartbeatLivenessPolicy>();
}

}  // namespace cme
