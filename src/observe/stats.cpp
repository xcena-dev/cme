// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// stats.cpp -- per-Event emit() overloads for the stats axis.
// CME_STATS defined -> real fetch_adds; undefined -> empty stubs (LTO elides).

#include "observe/stats.hpp"

#include <cstdint>

#include "common/ordered.hpp"
#include "common/timing.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/event.hpp"

#if defined(CME_STATS)
#include <algorithm>

#include "util/cpu.hpp"
#endif

namespace cme::stats
{

// Decode PeerTelemetry into a snapshot. Counters stay 0 when CME_STATS is off.
TelemetrySnapshot_t getTelemetrySnapshot(LocalPeerState& peerState) noexcept
{
    TelemetrySnapshot_t snapshot{};
    const auto& telemetry = peerState.getTelemetry();
    snapshot.waitCount = telemetry.ownership.count.arrivedSpin.load() +
                         telemetry.ownership.count.arrivedSleep.load() +
                         telemetry.ownership.count.notArrived.load();
    snapshot.waitTime = telemetry.ownership.time.wait.load();
    snapshot.spinTime = telemetry.ownership.time.spin.load();
    snapshot.lockHoldCount = telemetry.ownership.count.transferable.load();
    snapshot.lockHoldTime = telemetry.ownership.time.transferable.load();
    for (std::uint32_t domainId = 0; domainId < MaxDomains; ++domainId)
    {
        snapshot.perDomainAcquire[domainId] =
            telemetry.ownership.count.requested[domainId].load();
    }
    snapshot.transferOnRelease = telemetry.ownership.count.transferOnRelease.load();
    snapshot.transferOnPoll = telemetry.ownership.count.transferOnPoll.load();
    const auto& latency = telemetry.handoffLat;
    for (std::uint32_t bucket = 0; bucket < TelemetrySnapshot_t::LatCount; ++bucket)
    {
        snapshot.handoffLat.cycles[bucket] = latency.cycles[bucket].load();
        snapshot.handoffLat.count[bucket] = latency.count[bucket].load();
    }
    snapshot.recovery.takeover = telemetry.recovery.takeover.load();
    snapshot.recovery.completed = telemetry.recovery.completed.load();
    snapshot.recovery.rejoined = telemetry.recovery.rejoined.load();
#if defined(CME_STATS)
    snapshot.countersLive = true;
#endif
    return snapshot;
}

#if defined(CME_STATS)

namespace
{
inline void bump(ordered::Relaxed<std::uint64_t>& counter) noexcept
{
    counter.add(1);
}

// Accumulate wall + CPU wait time (shared by OwnershipArrived / NotArrived).
inline void recordWaitCommon(LocalPeerState& peerState, const timing::Stopwatch& waited,
                             timing::Nanos cpuStart) noexcept
{
    const auto wallElapsed = waited.elapsed();
    auto cpuElapsed = cpu::getThreadCpuTime() - cpuStart;
    if (cpuElapsed > wallElapsed)
    {
        cpuElapsed = wallElapsed;  // clamp: scheduler tick can inflate CPU proxy
    }
    auto& ownership = peerState.getTelemetry().ownership;
    ownership.time.wait.add(static_cast<NanosCount>(wallElapsed.count()));
    ownership.time.spin.add(static_cast<NanosCount>(cpuElapsed.count()));
}
}  // namespace

void emit(observe::EventTag_t<observe::Event::OwnershipRequested>,
          LocalPeerState& peerState, DomainId domainId) noexcept
{
    const std::uint32_t domainIndex = std::min<std::uint32_t>(domainId, MaxDomains - 1);
    peerState.getTelemetry().ownership.count.requested[domainIndex].add(1);
}

void emit(observe::EventTag_t<observe::Event::OwnershipAlreadyHave>,
          LocalPeerState& peerState) noexcept
{
    bump(peerState.getTelemetry().ownership.count.alreadyHave);
}

void emit(observe::EventTag_t<observe::Event::OwnershipArrived>,
          LocalPeerState& peerState, const timing::Stopwatch& waited,
          timing::Nanos cpuStart, bool isSpin) noexcept
{
    recordWaitCommon(peerState, waited, cpuStart);
    auto& ownership = peerState.getTelemetry().ownership;
    bump(isSpin ? ownership.count.arrivedSpin : ownership.count.arrivedSleep);
}

void emit(observe::EventTag_t<observe::Event::OwnershipNotArrived>,
          LocalPeerState& peerState, const timing::Stopwatch& waited,
          timing::Nanos cpuStart) noexcept
{
    recordWaitCommon(peerState, waited, cpuStart);
    bump(peerState.getTelemetry().ownership.count.notArrived);
}

void emit(observe::EventTag_t<observe::Event::OwnershipTransferable>,
          LocalPeerState& peerState, const timing::Stopwatch& held) noexcept
{
    const auto holdNs = held.elapsed();
    auto& ownership = peerState.getTelemetry().ownership;
    ownership.count.transferable.add(1);
    ownership.time.transferable.add(static_cast<NanosCount>(holdNs.count()));
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
          const timing::Stopwatch&, timing::Nanos, bool) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::OwnershipNotArrived>, LocalPeerState&,
          const timing::Stopwatch&, timing::Nanos) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::OwnershipTransferable>, LocalPeerState&, const timing::Stopwatch&) noexcept
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
