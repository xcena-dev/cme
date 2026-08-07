// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// stats.cpp -- per-Event emit() overloads for the stats axis.
// CME_STATS defined -> real fetch_adds; undefined -> empty stubs (LTO elides).

#include "observe/stats.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/event.hpp"
#include "util/time.hpp"

#if defined(CME_STATS)
#include <algorithm>
#endif

namespace cme::stats
{

// Decode PeerTelemetry into a snapshot. Counters stay 0 when CME_STATS is off.
TelemetrySnapshot_t getTelemetrySnapshot(LocalPeerState& peerState) noexcept
{
    TelemetrySnapshot_t snapshot{};
    const auto& telemetry = peerState.getTelemetry();
    snapshot.waitCount = telemetry.ownership.count.arrivedSpin.load(std::memory_order_relaxed) +
                         telemetry.ownership.count.arrivedSleep.load(std::memory_order_relaxed) +
                         telemetry.ownership.count.notArrived.load(std::memory_order_relaxed);
    snapshot.waitTime = telemetry.ownership.time.wait.load(std::memory_order_relaxed);
    snapshot.spinTime = telemetry.ownership.time.spin.load(std::memory_order_relaxed);
    snapshot.lockHoldCount = telemetry.ownership.count.transferable.load(std::memory_order_relaxed);
    snapshot.lockHoldTime = telemetry.ownership.time.transferable.load(std::memory_order_relaxed);
    for (std::uint32_t domainId = 0; domainId < MaxDomains; ++domainId)
    {
        snapshot.perDomainAcquire[domainId] =
            telemetry.ownership.count.requested[domainId].load(std::memory_order_relaxed);
    }
    snapshot.transferOnRelease = telemetry.ownership.count.transferOnRelease.load(std::memory_order_relaxed);
    snapshot.transferOnPoll = telemetry.ownership.count.transferOnPoll.load(std::memory_order_relaxed);
    const auto& latency = telemetry.handoffLat;
    for (std::size_t bucket = 0; bucket < TelemetrySnapshot_t::LatCount; ++bucket)
    {
        snapshot.handoffLat.cycles[bucket] = latency.cycles[bucket].load(std::memory_order_relaxed);
        snapshot.handoffLat.count[bucket] = latency.count[bucket].load(std::memory_order_relaxed);
    }
    snapshot.recovery.takeover = telemetry.recovery.takeover.load(std::memory_order_relaxed);
    snapshot.recovery.completed = telemetry.recovery.completed.load(std::memory_order_relaxed);
    snapshot.recovery.rejoined = telemetry.recovery.rejoined.load(std::memory_order_relaxed);
#if defined(CME_STATS)
    snapshot.countersLive = true;
#endif
    return snapshot;
}

#if defined(CME_STATS)

namespace
{
inline void bump(std::atomic<std::uint64_t>& counter) noexcept
{
    counter.fetch_add(1, std::memory_order_relaxed);
}

// Accumulate wall + CPU wait time (shared by OwnershipArrived / NotArrived).
inline void recordWaitCommon(LocalPeerState& peerState, time::TimePoint wallStart,
                             std::chrono::nanoseconds cpuStart) noexcept
{
    using std::chrono::duration_cast;
    const auto wallElapsed = duration_cast<std::chrono::nanoseconds>(time::getMonoTime() - wallStart);
    auto cpuElapsed = time::getThreadCpuTime() - cpuStart;
    if (cpuElapsed > wallElapsed)
    {
        cpuElapsed = wallElapsed;  // clamp: scheduler tick can inflate CPU proxy
    }
    auto& ownership = peerState.getTelemetry().ownership;
    ownership.time.wait.fetch_add(static_cast<time::nanoseconds>(wallElapsed.count()),
                                  std::memory_order_relaxed);
    ownership.time.spin.fetch_add(static_cast<time::nanoseconds>(cpuElapsed.count()),
                                  std::memory_order_relaxed);
}
}  // namespace

void emit(observe::EventTag_t<observe::Event::OwnershipRequested>,
          LocalPeerState& peerState, DomainId domainId) noexcept
{
    const std::uint32_t domainIndex = std::min<std::uint32_t>(domainId, MaxDomains - 1);
    peerState.getTelemetry().ownership.count.requested[domainIndex].fetch_add(1, std::memory_order_relaxed);
}

void emit(observe::EventTag_t<observe::Event::OwnershipAlreadyHave>,
          LocalPeerState& peerState) noexcept
{
    bump(peerState.getTelemetry().ownership.count.alreadyHave);
}

void emit(observe::EventTag_t<observe::Event::OwnershipArrived>,
          LocalPeerState& peerState, time::TimePoint wallStart,
          std::chrono::nanoseconds cpuStart, bool isSpin) noexcept
{
    recordWaitCommon(peerState, wallStart, cpuStart);
    auto& ownership = peerState.getTelemetry().ownership;
    bump(isSpin ? ownership.count.arrivedSpin : ownership.count.arrivedSleep);
}

void emit(observe::EventTag_t<observe::Event::OwnershipNotArrived>,
          LocalPeerState& peerState, time::TimePoint wallStart,
          std::chrono::nanoseconds cpuStart) noexcept
{
    recordWaitCommon(peerState, wallStart, cpuStart);
    bump(peerState.getTelemetry().ownership.count.notArrived);
}

void emit(observe::EventTag_t<observe::Event::OwnershipTransferable>,
          LocalPeerState& peerState, time::TimePoint holdStart) noexcept
{
    using std::chrono::duration_cast;
    const auto holdNs = duration_cast<std::chrono::nanoseconds>(time::getMonoTime() - holdStart);
    auto& ownership = peerState.getTelemetry().ownership;
    ownership.count.transferable.fetch_add(1, std::memory_order_relaxed);
    ownership.time.transferable.fetch_add(static_cast<time::nanoseconds>(holdNs.count()),
                                          std::memory_order_relaxed);
}

void emit(observe::EventTag_t<observe::Event::OwnershipTransferOnRelease>,
          LocalPeerState& peerState) noexcept
{
    bump(peerState.getTelemetry().ownership.count.transferOnRelease);
}

void emit(observe::EventTag_t<observe::Event::OwnershipTransferOnPoll>,
          LocalPeerState& peerState) noexcept
{
    bump(peerState.getTelemetry().ownership.count.transferOnPoll);
}

void emit(observe::EventTag_t<observe::Event::RecoveryTakeover>, LocalPeerState& peerState,
          DomainId) noexcept
{
    bump(peerState.getTelemetry().recovery.takeover);
}

void emit(observe::EventTag_t<observe::Event::RecoveryCompleted>, LocalPeerState& peerState,
          PeerId) noexcept
{
    bump(peerState.getTelemetry().recovery.completed);
}

#else  // CME_STATS undefined -- empty stubs

void emit(observe::EventTag_t<observe::Event::OwnershipRequested>, LocalPeerState&, DomainId) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::OwnershipAlreadyHave>, LocalPeerState&) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::OwnershipArrived>, LocalPeerState&,
          time::TimePoint, std::chrono::nanoseconds, bool) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::OwnershipNotArrived>, LocalPeerState&,
          time::TimePoint, std::chrono::nanoseconds) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::OwnershipTransferable>, LocalPeerState&, time::TimePoint) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::OwnershipTransferOnRelease>, LocalPeerState&) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::OwnershipTransferOnPoll>, LocalPeerState&) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::RecoveryTakeover>, LocalPeerState&, DomainId) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::RecoveryCompleted>, LocalPeerState&, PeerId) noexcept
{
}

#endif  // CME_STATS

}  // namespace cme::stats
