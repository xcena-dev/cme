// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// coherency.hpp -- Cross-host visibility fences (wmb / rmb).
//
// Bounds heartbeat/doorbell/record visibility latency (SWOT reliable-FD, Layer 2).
//
// The regime is a runtime value, because it is a property of how *this* peer mapped the
// region rather than of the region: two peers over one device may differ, one mapping devdax
// WB while the other reaches the same storage through a UC file. The Session takes it from
// its mapping. It reaches these calls through LocalPeerState, or as an explicit
// parameter on the paths that run before a LocalPeerState exists (format, admission).
//
// Costs measured on this bench (ns per 64B op, sustained; data_coherency_0728/):
//   lfence   free on UC (241.4 -> 241.4), +6.9 on a WB read (0.51 -> 7.44)
//   clflush  +94 on UC and buys nothing there, +320 on a WB read (0.51 -> 327)
//   the Mode load and its branch, +0.22 on a WB read, 0 on a write
// So three regimes rather than two: folding Uncached into CacheCoherent would charge shm a
// fence it does not need, and the branch that avoids it costs 30x less than the fence.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "cme/limits.hpp"  // CacheLineBytes: the unit every transaction below moves
#include "cme/shared.hpp"  // CoherencyMode: one type, set per Session at format/open

// Every regime now compiles into every build, so the whole file is x86-only. Kept as one
// guard rather than spread over the call sites: a port has a single place to look.
#if !defined(__x86_64__)
#error "cme currently requires x86 (clflush/clflushopt/sfence/mfence)"
#endif
#include <emmintrin.h>  // _mm_clflush, _mm_mfence, _mm_sfence
#include <immintrin.h>  // _mm_clflushopt, _movdir64b, _mm512_loadu_si512

namespace cme
{

// The line width as an address mask. Widened here rather than at each use, because inverting a
// 32-bit width gives a 32-bit mask and anding that with a 64-bit address clears its upper half.
inline constexpr std::uintptr_t LineOffsetMask = static_cast<std::uintptr_t>(CacheLineBytes) - 1;

// What a record has to satisfy to live in a region and be moved by get/set below: whole
// cachelines, so a write moves one line; trivially copyable, so get/set may move it as
// bytes; standard layout, so another build agrees on where its fields are.
template <typename T>
inline constexpr bool IsRegionRecord = std::is_standard_layout_v<T> &&
                                       std::is_trivially_copyable_v<T> &&
                                       sizeof(T) % CacheLineBytes == 0;

// ── Cross-host memory barriers ────────────────────────────────────
namespace coherency
{

// The Session's answer, spelled coherency::Mode at these call sites. Declared publicly
// because a format names it to pick a file pool; aliased rather than mirrored so
// there is one type and nothing to keep in sync.
using Mode = CoherencyMode;

// Publish to FAM before any later store.
inline void wmb(const void* addr, std::size_t len, Mode mode) noexcept
{
    std::atomic_thread_fence(std::memory_order_release);
    if (mode == Mode::CacheCoherent)
    {
        return;
    }
    if (mode == Mode::Flush)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(addr) & ~LineOffsetMask;
        const auto end = reinterpret_cast<std::uintptr_t>(addr) + len;
        // The walk is integer arithmetic because void* has none, and aligning a pointer below
        // its own object is out of bounds. Nothing dereferences what comes back.
        for (auto line = base; line < end; line += CacheLineBytes)
        {
            _mm_clflushopt(reinterpret_cast<void*>(line));  // NOLINT(performance-no-int-to-ptr)
        }
    }
    _mm_sfence();
}

// Drop a stale local copy, then order the read behind it.
inline void rmb(const void* addr, std::size_t len, Mode mode) noexcept
{
    if (mode != Mode::CacheCoherent)
    {
        if (mode == Mode::Flush)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(addr) & ~LineOffsetMask;
            const auto end = reinterpret_cast<std::uintptr_t>(addr) + len;
            // Integer walk for the same reason as wmb: void* has no arithmetic, and aligning a
            // pointer below its own object is out of bounds.
            for (auto line = base; line < end; line += CacheLineBytes)
            {
                _mm_clflush(reinterpret_cast<void*>(line));  // NOLINT(performance-no-int-to-ptr)
            }
        }
        // mfence, not lfence: lfence waits only for a flush to retire, so a later load can still hit
        // the line before the flush removes it. mfence waits for the flush to complete.
        _mm_mfence();
    }
    std::atomic_thread_fence(std::memory_order_acquire);
}

// Full barrier (StoreLoad) -- the one ordering x86 TSO does not give for free, and neither
// wmb nor rmb subsumes it. Pure CPU ordering, not a FAM-visibility flush, so it is
// regime-independent. Needed by the Peterson turn-store before the flag-load.
inline void mb() noexcept
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

// ── Whole-line moves ──────────────────────────────────────────────

// The instruction is fixed at compile time by what -march grants, so the hot path has no CPU check.
// A build without those flags gets the byte copy, which can tear.

// A zmm move is usually one transaction but promises nothing, and a byte copy is the last resort.
inline void copyLine(void* destination, const void* source) noexcept
{
#if defined(__AVX512F__)
    _mm512_storeu_si512(destination, _mm512_loadu_si512(source));
#else
    std::memcpy(destination, source, CacheLineBytes);
#endif
}

// Whole-64B slot transfer. SINGLE-WRITER SLOTS ONLY -- set() stores all 64B, overwriting a
// concurrent writer's field.
template <typename T>
[[nodiscard]] inline T get(const T* slot, Mode mode) noexcept
{
    static_assert(sizeof(T) == CacheLineBytes && IsRegionRecord<T>,
                  "coherency::get is for one-cacheline structs");
    rmb(slot, sizeof(T), mode);
    T value;
    copyLine(&value, slot);
    return value;
}

// MOVDIR64B is the one 64-byte store the architecture guarantees whole. It writes straight to
// memory and leaves nothing in this host's cache, so it takes the fence and no flush.
template <typename T>
inline void set(T* slot, const T& value, Mode mode) noexcept
{
    static_assert(sizeof(T) == CacheLineBytes && IsRegionRecord<T>,
                  "coherency::set is for one-cacheline structs");
#if defined(__MOVDIR64B__)
    _movdir64b(slot, &value);
    wmb(slot, sizeof(T), Mode::Uncached);
    static_cast<void>(mode);
#else
    copyLine(slot, &value);
    wmb(slot, sizeof(T), mode);
#endif
}

// get the whole slot, run @mutate on the copy, set it back whole. Not for paths that branch on
// a magic check -- mutate cannot bail out; use rmwIfTrue.
template <typename T, typename T_Mutate>
inline void rmw(T* slot, Mode mode, T_Mutate&& mutate) noexcept(noexcept(mutate(slot)))
{
    T copy = get(slot, mode);
    mutate(&copy);
    set(slot, copy, mode);
}

// Conditional RMW for the get -> precondition check -> mutate -> set pattern. A false from
// @mutate skips the set and leaves the slot unwritten, and the return value says which happened.
template <typename T, typename T_Mutate>
inline bool rmwIfTrue(T* slot, Mode mode, T_Mutate&& mutate) noexcept(noexcept(mutate(slot)))
{
    T copy = get(slot, mode);
    if (!mutate(&copy))
    {
        return false;
    }
    set(slot, copy, mode);
    return true;
}

}  // namespace coherency
}  // namespace cme
