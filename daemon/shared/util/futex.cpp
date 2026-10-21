// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/util/futex.cpp -- see futex.hpp.

#include "shared/util/futex.hpp"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <limits>

#include "common/ordered.hpp"
#include "common/spin.hpp"
#include "common/timing.hpp"

namespace cmed::util
{

namespace
{

// The syscall takes a plain word, and these hold the atomic to being that same object at that same
// address rather than assuming it.
static_assert(sizeof(ordered::SeqCst<std::uint32_t>) == sizeof(std::uint32_t), "atomic word is one word");
static_assert(alignof(ordered::SeqCst<std::uint32_t>) == alignof(std::uint32_t), "atomic word aligns as one");

[[nodiscard]] std::uint32_t* castToWord(ordered::SeqCst<std::uint32_t>& word) noexcept
{
    return reinterpret_cast<std::uint32_t*>(&word);
}

// No FUTEX_PRIVATE_FLAG: the waiter and the waker are different processes, and the private form
// keys on an address two mappings do not share.
[[nodiscard]] std::int64_t callFutexWait(ordered::SeqCst<std::uint32_t>& word,
                                         std::uint32_t expected,
                                         timing::Nanos timeout) noexcept
{
    const ::timespec span = timing::toTimespec(timeout);
    return ::syscall(SYS_futex, castToWord(word), FUTEX_WAIT, expected, &span, nullptr, 0);
}

std::int64_t callFutexWake(ordered::SeqCst<std::uint32_t>& word, std::int32_t sleepers) noexcept
{
    return ::syscall(SYS_futex, castToWord(word), FUTEX_WAKE, sleepers, nullptr, nullptr, 0);
}

// The first look stays cheap, and the backoff keeps a word nobody is about to move from being
// hammered. The cap stays low: a burst long enough to cover the whole answer delays the wake.
[[nodiscard]] bool spinOnWord(ordered::SeqCst<std::uint32_t>& word,
                              std::uint32_t expected,
                              timing::Nanos spin) noexcept
{
    constexpr std::uint32_t LeastPausesPerRound = 1;
    constexpr std::uint32_t MostPausesPerRound = 32;

    if (spin <= timing::Nanos::zero())
    {
        return false;
    }

    const timing::Deadline spinning{spin};
    spin::Backoff backoff{LeastPausesPerRound, MostPausesPerRound};

    while (true)
    {
        if (word.load() != expected)
        {
            return true;
        }

        backoff.pause();

        if (spinning.expired())
        {
            return false;
        }
    }
}

// Raises the flag for as long as it lives, and holds a reference because there is no not-recording
// state: a caller with nothing to record calls the overload that never builds one.
//
// seq_cst: the waker stores the value then reads this word, and either store left in a core's buffer
// could skip a wake the sleeper needed. The kernel's compare catches the other order.
struct Parked_t
{
    ordered::SeqCst<std::uint32_t>& word;

    explicit Parked_t(ordered::SeqCst<std::uint32_t>& flag) noexcept
        : word{flag}
    {
        word.store(1);
    }

    ~Parked_t() noexcept
    {
        word.store(0);
    }
};

// The futex loop alone, so a caller recording its sleep scopes that record to the sleep rather than to
// the spin ahead of it.
[[nodiscard]] bool
sleepOnWord(ordered::SeqCst<std::uint32_t>& word, std::uint32_t expected, const timing::Deadline& deadline) noexcept
{
    while (true)
    {
        const auto remaining = deadline.remaining();
        if (!remaining)
        {
            return false;
        }

        // Remaining time, not the original span: the kernel's timeout is relative and a signal ends
        // the wait without consuming it, so reusing the original would extend the deadline.
        if (callFutexWait(word, expected, *remaining) == 0)
        {
            return true;
        }

        // The word had already moved, so there is nothing to sleep through.
        if (errno == EAGAIN)
        {
            return true;
        }
        if (errno == ETIMEDOUT)
        {
            return false;
        }
        if (errno != EINTR)
        {
            // Nothing else is recoverable, and reporting it as a wake sends the caller back to read
            // the word, which is the safe direction.
            return true;
        }
    }
}

}  // namespace

bool waitOnWord(ordered::SeqCst<std::uint32_t>& word, std::uint32_t expected, timing::Nanos timeout,
                timing::Nanos spin) noexcept
{
    const timing::Deadline deadline{timeout};

    // Before the first syscall, and only once: a caller that comes back has already spent its spin on
    // the value it is now re-reading.
    if (spinOnWord(word, expected, spin))
    {
        return true;
    }

    return sleepOnWord(word, expected, deadline);
}

bool waitOnWord(ordered::SeqCst<std::uint32_t>& word, std::uint32_t expected, timing::Nanos timeout,
                timing::Nanos spin, ordered::SeqCst<std::uint32_t>& parked) noexcept
{
    const timing::Deadline deadline{timeout};

    // The flag stays down through the spin. What it says is that a sleeper is queued in the kernel, and
    // a spinning thread needs no wake to see the word move.
    if (spinOnWord(word, expected, spin))
    {
        return true;
    }

    const Parked_t sleeping{parked};
    return sleepOnWord(word, expected, deadline);
}

void wakeOneWaiter(ordered::SeqCst<std::uint32_t>& word) noexcept
{
    static_cast<void>(callFutexWake(word, 1));
}

void wakeAllWaiters(ordered::SeqCst<std::uint32_t>& word) noexcept
{
    // INT32_MAX is the documented way to say "every sleeper," not a cap this code chose; the width
    // is ours to state since syscall is variadic and declares no type.
    static_cast<void>(callFutexWake(word, std::numeric_limits<std::int32_t>::max()));
}

}  // namespace cmed::util
