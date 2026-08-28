// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/observe/counters.hpp -- what a run counted, and the word that counts it. Whoever owns a set of
// these answers one event at a time, so a count reaches a caller as a number and never as state.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "common/timing.hpp"

namespace cmed::observe
{

// What the domain side can be asked about. Local grants, and the cme acquires they cost. Cohorting is
// the gap between the two: a requester served on ownership already held is a grant with no acquire.
enum class DomainEvent : std::uint32_t
{
    Grants,
    Acquires,

    // Turns given back. Against Acquires it says whether the two balance: a run that acquires more
    // than it drops is holding domains against every other node.
    TurnsDropped,

    // The two refusals a requester gets, apart because only one of them is worth retrying. A turn cme
    // would not sell in time, against a name this node cannot reach at all.
    RefusedNoTurn,
    RefusedUnusable,

    // A slot the region gave to another name while this one still stood there. Zero by contract, so
    // any count at all names a window the refresh interval left open.
    SlotsBroken,

    // Slots a refresh took back, and passes it abandoned because the region would not answer. A run
    // whose region is flaky shows the second climbing while the table stops matching.
    SlotsRetired,
    RefreshesLost,

    Count,
};

// What one hand-over turned out to be. The pool's own enumeration, since every one of these is decided
// inside the call that hands a domain over rather than by the pass that made the call.
enum class WorkerEvent : std::uint32_t
{
    // Domains handed to a worker, and the two reasons a hand-over did not happen. Split, because a
    // summed count would not say which of the two to fix.
    Handed,
    DeferredBusy,
    DeferredNoWorker,

    // Hand-overs that cost a syscall, against those that found the worker awake and cost none.
    Woke,
    SkippedWake,

    Count,
};

// What a dispatcher pass turned out to be. Apart from WorkerEvent because those count hand-overs and a
// pass may make several or none, so neither number can be read off the other.
enum class DispatchEvent : std::uint32_t
{
    Passes,

    // The three ways a pass ends, in the order it decides them. A pass that handed anything comes
    // straight round, one that left a bit up spins its window, and one with nothing sleeps its turn.
    Handed,
    Deferred,
    Idle,

    Count,
};

// What the maintenance pass can be asked about. Apart from DomainEvent because those say what a sweep
// changed, and a pass that found nothing to change moves none of them while still having run.
enum class MaintainEvent : std::uint32_t
{
    Passes,

    // Passes that pinged the watchdog, against those that found the dispatcher had not moved since the
    // last one. Split, because the second is what a stalled dispatcher looks like from this thread.
    Alive,
    Silent,

    Count,
};

// What the control loop can be asked about. Its own enumeration, so a reading taken off a run that
// bound no socket cannot name one of these at all rather than being answered zero.
enum class ControlEvent : std::uint32_t
{
    // Arrivals taken onto the loop, against those that left it however they left. The difference is
    // how many are held now, which is what the bound below turns into a question worth asking: a
    // difference near it is what precedes the first refusal.
    Admitted,
    Departed,

    // Connections welcomed, against those dropped for saying something unfollowable. Apart from the
    // pair above, since a peer that connects and never greets is admitted and holds a descriptor.
    Welcomed,
    Misspoken,

    // Control requests answered, whatever the answer was. A refusal is an answer: the connection spoke
    // the protocol correctly and got a no.
    Answered,

    // Arrivals turned away at the connection bound, before any of the above. Rising here says the
    // bound is what a requester met, which no other count would tell apart from a daemon that is gone.
    Crowded,

    Count,
};

// The words themselves, one per event. Relaxed throughout: these order nothing, and a serving path
// that fenced for them would pay for the reading it is only being measured by.
template <typename T>
class EventCounts
{
public:
    void bump(T which) noexcept
    {
        counted_[static_cast<std::uint32_t>(which)].fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t read(T which) const noexcept
    {
        return counted_[static_cast<std::uint32_t>(which)].load(std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(T::Count)> counted_{};
};

// What a run spent, against how many times it spent it. Two words rather than a histogram: the mean
// is what says whether a path costs what its design says, and a distribution is a measurement's job.
enum class SpanEvent : std::uint32_t
{
    // One cme acquire, which is the round trip cohorting exists to amortise. Against DomainEvent's
    // Acquires it says what each of them cost.
    RemoteAcquire,

    // Not here: what one local hand-over costs. Measuring it needs the instant the requester raised
    // its bit, and the slot carries no such stamp, so it would take a field in the shared layout.

    // Giving the turn back, so a run whose drops are slow shows here rather than in the acquires the
    // next node then waits on.
    TurnDrop,

    Count,
};

// Relaxed like the counts beside it: nothing is ordered through these, and a serving path that fenced
// for them would pay for the reading it is only being measured by.
template <typename T>
class SpanSums
{
public:
    void add(T which, timing::Nanos spent) noexcept
    {
        const auto index = static_cast<std::uint32_t>(which);
        // A span already past reads as zero rather than as the huge number the cast would make of it.
        const auto taken = spent > timing::Nanos::zero() ? static_cast<std::uint64_t>(spent.count()) : 0;

        nanos_[index].fetch_add(taken, std::memory_order_relaxed);
        times_[index].fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t readNanos(T which) const noexcept
    {
        return nanos_[static_cast<std::uint32_t>(which)].load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t readTimes(T which) const noexcept
    {
        return times_[static_cast<std::uint32_t>(which)].load(std::memory_order_relaxed);
    }

    // Zero when nothing was measured, which reads the same as a path that cost nothing. A caller that
    // needs the two apart asks readTimes.
    [[nodiscard]] std::uint64_t readMeanNanos(T which) const noexcept
    {
        const std::uint64_t times = readTimes(which);
        return times == 0 ? 0 : readNanos(which) / times;
    }

private:
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(T::Count)> nanos_{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(T::Count)> times_{};
};

}  // namespace cmed::observe
