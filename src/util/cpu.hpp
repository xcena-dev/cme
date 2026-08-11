// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cpu.hpp -- what a waiting thread does with the CPU, and what the CPU can tell it.
//
// Spans and budgets go through timing::, and wall time through timing::wall. What is left is the
// processor itself: its cycle counter, its pause hint, the backoff that feeds them, and the CPU
// time this thread has actually burned.

#pragma once

// clock_gettime and CLOCK_THREAD_CPUTIME_ID are POSIX, and <ctime> promises neither, so the C
// header is the one that has them.
#include <time.h>  // NOLINT(modernize-deprecated-headers)

#include <chrono>
#include <cstdint>
#include <thread>

#include "common/timing.hpp"

namespace cme::cpu
{

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
[[nodiscard]] inline timing::Nanos getThreadCpuTime() noexcept
{
    struct timespec now;
#ifdef CLOCK_THREAD_CPUTIME_ID
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
#else
    clock_gettime(CLOCK_MONOTONIC, &now);
#endif
    return timing::Secs{now.tv_sec} + timing::Nanos{now.tv_nsec};
}

}  // namespace cme::cpu
