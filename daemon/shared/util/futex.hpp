// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/util/futex.hpp -- sleeping on a word in the shared area.
//
// The requester waits on a domain's seq and the daemon waits on the doorbell, and both need the
// same rule for when a sleep may start.
//
// FUTEX_WAIT compares the word to @expected while holding the kernel's queue lock, so a store that
// lands between the caller's read and its decision to sleep cannot be missed.

#pragma once

#include <atomic>
#include <cstdint>

#include "common/timing.hpp"

namespace cmed::util
{

// How long a waiter spins before sleeping. Answers on these words arrive in single-digit
// microseconds, and a sleep costs a wake and a reschedule at each end that a spin this long avoids.
inline constexpr timing::Nanos SpinBeforeSleep{timing::Micros{200}};

// Returns false only on deadline; a spurious wake, a real wake, and a value already moved all return
// true, since all three mean "look again" to the caller. Zero @spin skips the spin entirely.
[[nodiscard]] bool waitOnWord(std::atomic<std::uint32_t>& word,
                              std::uint32_t expected,
                              timing::Nanos timeout,
                              timing::Nanos spin = SpinBeforeSleep) noexcept;

// Raises @parked while this call is queued in the kernel, and only then: a spinning thread is not one a
// waker has to reach. For a waker that skips the syscall when nobody is parked.
[[nodiscard]] bool waitOnWord(std::atomic<std::uint32_t>& word,
                              std::uint32_t expected,
                              timing::Nanos timeout,
                              timing::Nanos spin,
                              std::atomic<std::uint32_t>& parked) noexcept;

// Wakes every sleeper. The words here are contended by a handful of local requesters at most, so
// waking one at a time would buy nothing and would need the caller to know how many are queued.
void wakeAllWaiters(std::atomic<std::uint32_t>& word) noexcept;

// Wakes one sleeper, for a word whose waiters are interchangeable and whose work is one item; the
// rest would just wake, find it taken, and sleep again. Wrong choice when waiters wait for different reasons.
void wakeOneWaiter(std::atomic<std::uint32_t>& word) noexcept;

}  // namespace cmed::util
