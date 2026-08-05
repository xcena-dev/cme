// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper.hpp -- pieces a case wants whatever medium it runs on: randomness, waiting,
// summary statistics, and seeding a formatted region with data domains.
//
// Header-only: each test is its own binary, so the inline functions resolve to one
// instance per executable -- no helper.cpp to link. What a *run* is lives in
// test_context.hpp; nothing here reads the harness flags.
//
// In namespace harness with the rest of the shared test code, which is also what keeps `log` from
// sharing the global namespace with ::log(double) from <math.h>.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"

namespace harness
{

// xorshift64. Per-thread state, so the sweeps stay independent without sharing.
inline std::uint64_t nextRandom(std::uint64_t& state) noexcept
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Fisher-Yates over a thread's domain visit order. A permutation, so every domain is still
// locked exactly once per sweep -- only the lockstep between threads is broken.
inline void shuffleVisitOrder(std::vector<cme::DomainId>& visit, std::uint64_t& rng) noexcept
{
    for (auto i = static_cast<std::uint32_t>(visit.size()); i > 1; --i)
    {
        std::swap(visit[i - 1], visit[nextRandom(rng) % i]);
    }
}

// Where the timestamps on log lines are measured from. Set at static init so a case that
// never calls startLogClock() still prints a sane elapsed time rather than time since the
// epoch.
inline std::chrono::steady_clock::time_point& logOrigin()
{
    static std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    return origin;
}

// Re-base the clock on the moment a run actually starts, past the harness setup.
inline void startLogClock()
{
    logOrigin() = std::chrono::steady_clock::now();
}

// A timestamped line. A recovery case is a timeline, and the interesting question is
// usually how long after the freeze something happened, so the elapsed seconds lead.
inline void log(const char* fmt, ...)
{
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - logOrigin()).count();
    std::printf("[t=%6.2fs] ", elapsed);

    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);

    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Sleep in the unit every test here thinks in. A chrono literal in the middle of a recovery
// sequence reads as noise; the number is the part that matters.
inline void sleepMs(std::uint32_t milliSeconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds{milliSeconds});
}

// How often waitUntil re-checks. Long enough that the polling is not itself the load, short
// enough that a transition lands in the right frame.
inline constexpr std::uint32_t PollStepMs = 200;

// Poll @pred until it holds or @deadlineMs elapses; returns whether it held.
//
// A test waits on state another peer publishes, and a fixed sleep long enough to be safe is
// also long enough to hide the timing under test. Polling to a deadline keeps the wait as
// short as the system allows and still bounds it.
template <typename T_Pred>
[[nodiscard]] bool waitUntil(T_Pred pred, std::uint32_t deadlineMs,
                             std::uint32_t stepMs = PollStepMs)
{
    for (std::uint32_t waited = 0; waited < deadlineMs; waited += stepMs)
    {
        if (pred())
        {
            return true;
        }
        sleepMs(stepMs);
    }
    return pred();
}

// Whether @body threw T_Exc. Any other exception propagates rather than being reported as
// "did not throw", so a case that throws the wrong type fails where it happened.
template <typename T_Exc, typename T_Body>
[[nodiscard]] bool threw(T_Body body)
{
    try
    {
        body();
    }
    catch (const T_Exc&)
    {
        return true;
    }
    return false;
}

// Value at @fraction of a sample the caller has already sorted.
[[nodiscard]] inline double percentile(const std::vector<std::uint32_t>& sorted, double fraction)
{
    if (sorted.empty())
    {
        return 0.0;
    }
    const auto index = static_cast<std::uint64_t>(fraction * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[index]);
}

// Seed @count data domains into slots 1..count via a transient peer 0; genesis ownership
// stays on slot 0 and the real peer 0 re-adopts it. Call once after format.
inline void seedDataDomains(cme::Geometry& region, std::uint32_t count,
                            cme::CoherencyMode coherency)
{
    cme::Peer creator{region, 0, coherency};
    for (std::uint32_t i = 0; i < count; ++i)
    {
        (void)creator.createDomain("lane" + std::to_string(i));
    }
}

}  // namespace harness
