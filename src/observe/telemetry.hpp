// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// telemetry.hpp -- PeerTelemetry_t: per-peer atomic counters, one group per Event group.
// All fields always allocated; CME_STATS/CME_LOGGING stubs control whether bumps happen.
// Poll thread mirrors ownership.time onto MemberProfile_t for cross-node visibility.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/types.hpp"
#include "observe/latency.hpp"
#include "util/time.hpp"

namespace cme
{

struct PeerTelemetry_t
{
    // ── Ownership events (Event::Ownership*) ─────────────────────
    struct
    {
        struct
        {
            // OwnershipRequested (per-domain)
            std::atomic<std::uint64_t> requested[MaxDomains]{};
            // OwnershipAlreadyHave
            std::atomic<std::uint64_t> alreadyHave{0};
            // OwnershipArrived (spin/sleep variants)
            std::atomic<std::uint64_t> arrivedSpin{0};
            std::atomic<std::uint64_t> arrivedSleep{0};
            // OwnershipNotArrived
            std::atomic<std::uint64_t> notArrived{0};
            // OwnershipTransferable
            std::atomic<std::uint64_t> transferable{0};
            // OwnershipTransferOnRelease / OnPoll
            std::atomic<std::uint64_t> transferOnRelease{0};
            std::atomic<std::uint64_t> transferOnPoll{0};
        } count;
        struct
        {
            std::atomic<time::nanoseconds> wait{0};          // wait wall
            std::atomic<time::nanoseconds> spin{0};          // wait CPU (busy-spin proxy)
            std::atomic<time::nanoseconds> transferable{0};  // lock-hold duration
        } time;
    } ownership;

    // ── Recovery FSM events (Event::Recovery*) ───────────────────
    struct
    {
        std::atomic<std::uint64_t> takeover{0};
        std::atomic<std::uint64_t> completed{0};
        std::atomic<std::uint64_t> rejoined{0};
    } recovery;

    // ── Handoff latency breakdown (CME_LATENCY) ───────────────────
    // Raw TSC cycles + sample count per segment of release->next-acquire, indexed by
    // trace::LatencyStage and charged to whichever peer ran the segment.
    static constexpr std::size_t LatCount = static_cast<std::size_t>(trace::LatencyStage::Count);
    struct
    {
        std::atomic<std::uint64_t> cycles[LatCount]{};
        std::atomic<std::uint64_t> count[LatCount]{};
    } handoffLat;
};

}  // namespace cme
