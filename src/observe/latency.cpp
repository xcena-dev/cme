// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// latency.cpp -- global lane registry + JSONL dump for CME_LATENCY.

// Its own header, so the include stays whatever the build says. With CME_LATENCY off the rest of
// this file compiles to nothing and include-cleaner then reads the include as unused.
#include "observe/latency.hpp"  // NOLINT(misc-include-cleaner)

#if defined(CME_LATENCY)

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

namespace cme::trace
{
namespace
{

// A measured span [tscBegin, tscEnd] tagged with its LatencyStage.
struct SpanRecord_t
{
    std::uint64_t tscBegin;
    std::uint64_t tscEnd;
    std::uint16_t domain;
    std::uint8_t stageCode;  // LatencyStage value
    bool measured;           // false during warmup, true after setMeasuring(true)
};

struct Lane_t
{
    std::uint16_t peerId;
    std::uint8_t roleCode;
    std::uint32_t threadId;
    std::vector<SpanRecord_t> records;
};

std::mutex g_mutex;
std::vector<Lane_t> g_lanes;
std::uint32_t g_nextThreadId = 0;

// Per-thread buffer; flushes into g_lanes on thread exit so the bench (a
// different thread) can read it after join.
struct ThreadLocalBuffer_t
{
    std::vector<SpanRecord_t> records;
    std::uint16_t peerId = 0;
    std::uint8_t roleCode = 0;
    std::uint32_t threadId = 0;
    bool isTagged = false;

    void tag(std::uint16_t laneOwner, std::uint8_t laneRole) noexcept
    {
        if (isTagged)
        {
            return;
        }
        peerId = laneOwner;
        roleCode = laneRole;
        records.reserve(1u << 16);
        const std::lock_guard<std::mutex> guard{g_mutex};
        threadId = g_nextThreadId++;
        isTagged = true;
    }

    void flush() noexcept
    {
        if (records.empty())
        {
            return;
        }
        const std::lock_guard<std::mutex> guard{g_mutex};
        g_lanes.push_back(Lane_t{peerId, roleCode, threadId, std::move(records)});
        records.clear();
    }

    ~ThreadLocalBuffer_t()
    {
        flush();
    }
};

thread_local ThreadLocalBuffer_t g_threadBuffer;
thread_local bool g_threadMeasuring = false;  // set true by setMeasuring after warmup

const char* stageName(std::uint8_t stageCode) noexcept
{
    return getLatencyName(static_cast<LatencyStage>(stageCode));
}

}  // namespace

void setMeasuring(bool measuring) noexcept
{
    g_threadMeasuring = measuring;
}

void pushSpan(std::uint16_t peerId, LatencyStage stage, std::uint16_t domain,
              std::uint64_t tscBegin, std::uint64_t tscEnd) noexcept
{
    g_threadBuffer.tag(peerId, static_cast<std::uint8_t>(Role::Worker));
    g_threadBuffer.records.push_back(
        SpanRecord_t{tscBegin, tscEnd, domain, static_cast<std::uint8_t>(stage), g_threadMeasuring});
}

void writeJsonl(const char* path, double clockGhz) noexcept
{
    g_threadBuffer.flush();  // in case the caller logged anything
    const std::lock_guard<std::mutex> guard{g_mutex};
    FILE* file = std::fopen(path, "w");
    if (file == nullptr)
    {
        return;
    }
    std::fprintf(file, "{\"meta\":{\"ghz\":%.6f}}\n", clockGhz);
    for (const auto& lane : g_lanes)
    {
        const char* roleName = lane.roleCode == static_cast<std::uint8_t>(Role::Poll) ? "poll" : "worker";
        for (const auto& record : lane.records)
        {
            std::fprintf(file,
                         "{\"peer\":%u,\"role\":\"%s\",\"tid\":%u,\"kind\":\"span\","
                         "\"stage\":\"%s\",\"domain\":%u,\"measured\":%s,\"t0\":%" PRIu64
                         ",\"t1\":%" PRIu64 "}\n",
                         lane.peerId, roleName, lane.threadId, stageName(record.stageCode), record.domain,
                         record.measured ? "true" : "false", record.tscBegin, record.tscEnd);
        }
    }
    std::fclose(file);
}

}  // namespace cme::trace

#endif
