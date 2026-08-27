// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// spin.hpp -- what a thread does with the CPU while it waits for a word to move.
//
// The hint and the schedule that decides how many to emit are one type. Kept together because the
// two mistakes a caller makes with two of them are emitting a count nobody computed and computing
// one nobody emits.
//
// Header-only and free of both libraries, so the library and the daemon spin the same way rather
// than each keeping a copy of the same loop.

#pragma once

#include <cstdint>

namespace spin
{

// Doubling backoff over the CPU's pause hint. A wait that drags then reads the word less often,
// which is what keeps a spin from filling the interconnect with retries nobody answers.
class Backoff
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // @least is the first count and @most the ceiling. Equal bounds hold the count flat, for a
    // caller that measured a fixed rate to be the better one.
    Backoff(std::uint32_t least, std::uint32_t most) noexcept
        : pauses_{least < most ? least : most},
          most_{most}
    {
    }

    // ── public methods ─────────────────────────────────────────────────
    // Emits this round's hints, then sets the next round's count. A caller that stops spinning after
    // this call has already had the wait it asked for.
    void pause() noexcept
    {
        for (std::uint32_t hint = 0; hint < pauses_; ++hint)
        {
            relaxOnce();
        }
        pauses_ = pauses_ * 2 < most_ ? pauses_ * 2 : most_;
    }

private:
    // Tells the core it is spinning so the pipeline stops speculating past the load. A target with
    // no hint of its own emits nothing, since what the loop rests on is re-reading the word.
    static void relaxOnce() noexcept
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield" ::: "memory");
#endif
    }

private:
    std::uint32_t pauses_;
    std::uint32_t most_;
};

}  // namespace spin
