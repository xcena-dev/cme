// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// latency.hpp -- handoff-latency breakdown + per-thread event trace (CME_LATENCY).
//
// The data model (LatencyStage enum, getLatencyName) is always defined -- telemetry indexes
// it either way. The instrument is CME_LATENCY-only: END charges a counter and emits a trace
// span in one shot, so bar widths match the counters. Spans buffer per thread and flush to a
// global lane registry at thread exit, dumped as JSONL (see tests/sweep/latency_trace.py).
// OFF => every macro vanishes, no rdtsc, zero overhead.

#pragma once

#include <cstddef>
#include <cstdint>

namespace cme::trace
{

// Every instrumented segment of the handoff path. Enum value, telemetry index, and label all
// derive from this one list, so they cannot drift, and a bar reads as the macro that emits
// it. Order is load-bearing (array-indexed telemetry): append only, keep Count last.
#define CME_LATENCY_STAGES(X)                                                                          \
    X(Hold)          /* policy lock: holdDomain */                                                     \
    X(Raise)         /* request lock: raise doorbell + pending */                                      \
    X(Scan)          /* unlock: findSuccessor / transferToRequester */                                 \
    X(ClimbAnnounce) /* lockNode: interested+turn set + mb */                                          \
    X(ClimbSpin)     /* lockNode: busy-spin loop */                                                    \
    X(Release)       /* peterson unlock: clearDoorbell+winner+tree */                                  \
    X(Unlock)        /* unlock(): full release path */                                                 \
    X(Fence)         /* peer.cpp lock(): validate + isFenced rmb */                                    \
    X(Spin)          /* waitForOwnership: entry -> doorbell edge seen */                               \
    X(AdoptLocal)    /* waitForOwnership: becomeHolder bookkeeping */                                  \
    X(Clear)         /* unlock: clearOwnDoorbell */                                                    \
    X(Resident)      /* lock fast-path: holdAndCheckResident hit (no contention) */                    \
    X(Takeover)      /* peterson lock: takeoverOwnership after tournament win */                       \
    X(SpinPoll)      /* waitForOwnership: count of poll iterations (n=total spins, avg ns per poll) */ \
    X(Acquire)       /* Peer::lock(): whole acquire (Fence + policy lock) — outer span, others nest; release side = Unlock */

// Defined unconditionally: telemetry arrays + the breakdown readout index it even
// when CME_LATENCY is off.
enum class LatencyStage : std::uint8_t
{
#define CME_LATENCY_ENUMERATOR(name) name,
    CME_LATENCY_STAGES(CME_LATENCY_ENUMERATOR)
#undef CME_LATENCY_ENUMERATOR
    Count,
};

// Labels = stringized enum names, indexed by LatencyStage. Same list as the enum,
// so index and spelling are guaranteed to match (the picture label is the macro
// argument verbatim).
inline constexpr const char* LatencyNames[] = {
#define CME_LATENCY_NAME(name) #name,
    CME_LATENCY_STAGES(CME_LATENCY_NAME)
#undef CME_LATENCY_NAME
};

static_assert(sizeof(LatencyNames) / sizeof(LatencyNames[0]) == static_cast<std::size_t>(LatencyStage::Count),
              "LatencyNames out of sync with LatencyStage");

[[nodiscard]] inline const char* getLatencyName(LatencyStage stage) noexcept
{
    const auto index = static_cast<std::size_t>(stage);
    return index < static_cast<std::size_t>(LatencyStage::Count) ? LatencyNames[index] : "?";
}

}  // namespace cme::trace

#if defined(CME_LATENCY)

#include "util/cpu.hpp"

namespace cme::trace
{

// Lane role. Only worker threads emit spans today, so every lane tags Worker;
// the field is kept so the JSONL/plotter schema can grow poll lanes later.
enum class Role : std::uint8_t
{
    Worker = 0,
    Poll = 1,
};

void pushSpan(std::uint16_t peerId, LatencyStage stage, std::uint16_t domain,
              std::uint64_t tscBegin, std::uint64_t tscEnd) noexcept;

// Tag subsequent spans on THIS thread as measured (true) or warmup (false).
// The harness calls setMeasuring(true) after its warm-up barrier.
void setMeasuring(bool on) noexcept;

// Dump all lanes as JSONL to @path; first line is {"meta":{"ghz":...}} for ns
// conversion. Flushes the calling thread's buffer first. Call after all worker
// and poll threads have exited (their buffers flush on thread exit).
void writeJsonl(const char* path, double clockGhz) noexcept;

}  // namespace cme::trace

// stage is a bare LatencyStage value name (e.g. Hold); the macro adds the qualifier.
#define CME_TRACE_SPAN(peerId, stage, domain, tscBegin, tscEnd)                                 \
    cme::trace::pushSpan(static_cast<std::uint16_t>(peerId), ::cme::trace::LatencyStage::stage, \
                         static_cast<std::uint16_t>(domain), (tscBegin), (tscEnd))

// Internal: stamp the paste-built TSC local. maybe_unused: END's two consumers
// (counter add + trace span) don't both reference it in every inlining.
#define OBSERVE_LATENCY_TSC(tscLocal) [[maybe_unused]] const std::uint64_t tscLocal = cme::cpu::readTimestampCounter()
// Open a segment named after a LatencyStage value (paste-built begin local).
#define OBSERVE_LATENCY_BEGIN(stage) OBSERVE_LATENCY_TSC(_latencyTsc_##stage##_begin)

// stage = bare LatencyStage value name (e.g. Hold); the macro adds the qualifier and
// indexes the handoffLat arrays. Mirrors the OBSERVE_EVENT(Event::X) style.
#define CME_LAT_ADD(peerState, stage, deltaCycles)                                                  \
    do                                                                                              \
    {                                                                                               \
        auto& handoffLat = (peerState).getTelemetry().handoffLat;                                   \
        const std::size_t stageIndex = static_cast<std::size_t>(::cme::trace::LatencyStage::stage); \
        handoffLat.cycles[stageIndex].fetch_add((deltaCycles), std::memory_order_relaxed);          \
        handoffLat.count[stageIndex].fetch_add(1, std::memory_order_relaxed);                       \
    } while (0)

// Close the segment opened by OBSERVE_LATENCY_BEGIN(stage): charge counter + draw
// span in one shot.
#define OBSERVE_LATENCY_END(stage, peerState, domain)                                           \
    do                                                                                          \
    {                                                                                           \
        OBSERVE_LATENCY_TSC(_latencyTsc_##stage##_end);                                         \
        CME_LAT_ADD(peerState, stage, _latencyTsc_##stage##_end - _latencyTsc_##stage##_begin); \
        CME_TRACE_SPAN((peerState).getPeerId(), stage, (domain),                                \
                       _latencyTsc_##stage##_begin, _latencyTsc_##stage##_end);                 \
    } while (0)

// Count-only close: charge the counter but emit NO trace span. For hot inner loops
// (e.g. SpinPoll fires once per poll iteration) whose per-span records would flood
// the JSONL — the aggregate counter still carries n and mean.
#define OBSERVE_LATENCY_END_COUNT(stage, peerState)                                             \
    do                                                                                          \
    {                                                                                           \
        OBSERVE_LATENCY_TSC(_latencyTsc_##stage##_end);                                         \
        CME_LAT_ADD(peerState, stage, _latencyTsc_##stage##_end - _latencyTsc_##stage##_begin); \
    } while (0)

#else  // !CME_LATENCY -- every instrument macro vanishes, zero overhead.

namespace cme::trace
{
inline void writeJsonl(const char*, double) noexcept
{
}
inline void setMeasuring(bool) noexcept
{
}
}  // namespace cme::trace

#define CME_TRACE_SPAN(peerId, stage, domain, tscBegin, tscEnd) ((void)0)
#define OBSERVE_LATENCY_TSC(tscLocal)
#define OBSERVE_LATENCY_BEGIN(stage)
#define CME_LAT_ADD(peerState, stage, deltaCycles) ((void)0)
#define OBSERVE_LATENCY_END(stage, peerState, domain) ((void)0)
#define OBSERVE_LATENCY_END_COUNT(stage, peerState) ((void)0)

#endif
