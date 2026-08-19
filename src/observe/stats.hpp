// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// stats.hpp -- per-Event emit() overloads that bump PeerTelemetry counters.
// CME_STATS gate in stats.cpp; header is invariant.

#pragma once

#include <cstddef>
#include <cstdint>

#include "common/timing.hpp"
#include "core/types.hpp"
#include "observe/event.hpp"
#include "observe/latency.hpp"

namespace cme
{
struct LocalPeerState;

// Non-atomic best-effort snapshot of PeerTelemetry (DESIGN sec 10); cumulative.
struct TelemetrySnapshot_t
{
    std::uint64_t waitCount;  // Arrived(Spin+Sleep) + NotArrived
    NanosCount waitTime;
    NanosCount spinTime;
    std::uint64_t lockHoldCount;
    NanosCount lockHoldTime;
    std::uint64_t perDomainAcquire[MaxDomains];  // capped at MaxDomains

    // Handoff-latency breakdown: raw TSC cycles + sample count per segment.
    // All zero unless built with CME_LATENCY. Convert via measured tsc_khz.
    // transferOnRelease/Poll: which path actually hands the token off.
    std::uint64_t transferOnRelease, transferOnPoll;

    // Indexed by trace::LatencyStage; mirrors PeerTelemetry::handoffLat.
    static constexpr std::size_t LatCount = static_cast<std::size_t>(trace::LatencyStage::Count);
    struct
    {
        std::uint64_t cycles[LatCount];
        std::uint64_t count[LatCount];
    } handoffLat;

    // What this peer did as a recovery authority. completed is the executor's terminal act, so its
    // sum across live peers is how many of them ran a given recovery.
    struct
    {
        std::uint64_t takeover;
        std::uint64_t completed;
        std::uint64_t rejoined;
    } recovery;

    // Whether the build bumps any of the above. False = every count is zero for want of counting.
    bool countersLive;
};

namespace stats
{

// Decode PeerTelemetry into a snapshot; all-zero when CME_STATS is off.
[[nodiscard]] TelemetrySnapshot_t getTelemetrySnapshot(LocalPeerState& peerState) noexcept;

// Primary fallback: no-op for any Event without a specific overload.
template <observe::Event event, typename... T_Args>
inline void emit(observe::EventTag_t<event>, T_Args&&...) noexcept
{
}

// ── Ownership events ─────────────────────────────────────────────
void emit(observe::EventTag_t<observe::Event::OwnershipRequested>,
          LocalPeerState& peerState, DomainId domainId) noexcept;
void emit(observe::EventTag_t<observe::Event::OwnershipAlreadyHave>,
          LocalPeerState& peerState) noexcept;
void emit(observe::EventTag_t<observe::Event::OwnershipArrived>,
          LocalPeerState& peerState, const timing::Stopwatch& waited,
          timing::Nanos cpuStart, bool isSpin) noexcept;
void emit(observe::EventTag_t<observe::Event::OwnershipNotArrived>,
          LocalPeerState& peerState, const timing::Stopwatch& waited,
          timing::Nanos cpuStart) noexcept;
void emit(observe::EventTag_t<observe::Event::OwnershipTransferable>,
          LocalPeerState& peerState, const timing::Stopwatch& held) noexcept;
void emit(observe::EventTag_t<observe::Event::OwnershipTransferOnRelease>,
          LocalPeerState& peerState) noexcept;
void emit(observe::EventTag_t<observe::Event::OwnershipTransferOnPoll>,
          LocalPeerState& peerState) noexcept;

// ── Recovery FSM events ──────────────────────────────────────────
void emit(observe::EventTag_t<observe::Event::RecoveryTakeover>,
          LocalPeerState& peerState, DomainId domainId) noexcept;
void emit(observe::EventTag_t<observe::Event::RecoveryCompleted>,
          LocalPeerState& peerState, PeerId deadPeerId) noexcept;

}  // namespace stats
}  // namespace cme
