// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cpu.hpp -- what the CPU can tell a thread, and the sleep that hands the core back.
//
// Spans and budgets go through timing::, wall time through timing::wall, and the pause hint a
// spinning thread emits through spin::Backoff. What is left is what the processor can report: its
// cycle counter and the CPU time this thread has actually burned.

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
