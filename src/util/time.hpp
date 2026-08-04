// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// time.hpp -- steady_clock wall-time + THREAD_CPUTIME_ID CPU helpers.

#pragma once

#include <time.h>

#include <chrono>
#include <cstdint>
#include <thread>

namespace cme::time
{

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

// Raw u64 ns count (fits std::atomic<>); convert via std::chrono::nanoseconds{value}.
using nanoseconds = std::uint64_t;

[[nodiscard]] inline TimePoint getMonoTime() noexcept
{
    return SteadyClock::now();
}

// Wall-clock (CLOCK_REALTIME) ns since epoch, the liveness timestamp witness.
// Steppable; slew-only discipline assumed (HEARTBEAT design §2.4). Cross-node
// comparisons hold under the declared skew bound δ (ClockSkewBound).
[[nodiscard]] inline nanoseconds clockNowNanos() noexcept
{
    return static_cast<nanoseconds>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// Raw TSC cycles for the sub-us handoff breakdown. Assumes invariant/synced TSC
// (single-socket constant_tsc+nonstop_tsc x86), without which cross-thread deltas are
// meaningless; non-x86 returns 0. Convert to ns via measured tsc_khz.
[[nodiscard]] inline std::uint64_t readTimestampCounter() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

template <typename Rep, typename Period>
inline void sleepFor(std::chrono::duration<Rep, Period> duration) noexcept
{
    std::this_thread::sleep_for(duration);
}

// Emit `count` CPU PAUSE/yield hints (short hot-spin backoff; not a scheduler
// yield). count==0 is a no-op.
inline void relaxCpu(std::uint32_t count = 1) noexcept
{
    for (std::uint32_t i = 0; i < count; ++i)
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield" ::: "memory");
#endif
    }
}

// Doubling backoff for hot spins: next() returns the current PAUSE count, then
// doubles it (capped at max). Feed the result to relaxCpu.
class SpinBackoff
{
public:
    SpinBackoff(std::uint32_t min, std::uint32_t max) noexcept
        : pause_(min < max ? min : max),
          max_(max)
    {
    }

    std::uint32_t next() noexcept
    {
        const std::uint32_t cur = pause_;
        pause_ = pause_ * 2 < max_ ? pause_ * 2 : max_;
        return cur;
    }

private:
    std::uint32_t pause_;
    std::uint32_t max_;
};

// Per-thread CPU time; use as a delta (no fixed epoch).
[[nodiscard]] inline std::chrono::nanoseconds getThreadCpuTime() noexcept
{
    struct timespec now;
#ifdef CLOCK_THREAD_CPUTIME_ID
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
#else
    clock_gettime(CLOCK_MONOTONIC, &now);
#endif
    return std::chrono::seconds{now.tv_sec} + std::chrono::nanoseconds{now.tv_nsec};
}

}  // namespace cme::time
