// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/serve/workers.hpp -- the threads that do a domain's work, and the bit that says which domain is
// already being done. The dispatcher never blocks: a region acquire happens here instead, so one
// busy acquire cannot hold every other domain hostage. The in-flight mask keeps each domain with only one worker.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "common/bitmap.hpp"
#include "common/timing.hpp"
#include "daemon/observe/counters.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::daemon
{

// Named and not included: a worker calls into it, and the call is in the .cpp. An includer of this
// header takes the pool without taking the region behind it.
class DomainManager;

// One worker and everything it turns with, shaped in the .cpp: what a hand-over writes is the pool's
// own business, and the array only needs the shape where it is destroyed.
struct Worker_t;

class DomainWorkers
{
public:
    // One bit per worker in one word, so this is the ceiling on the pool. Past MaxDomains a worker could
    // never be handed anything of its own, so the two bounds meet without either being the other's.
    static constexpr std::uint32_t MostWorkers = 64;

    // ── ctor / dtor ────────────────────────────────────────────────
    // @count threads, each serving @domains the ids it is given. @spin is how long an idle worker
    // reads its own word before it sleeps on it. @count past MostWorkers is refused.
    DomainWorkers(DomainManager& domains, std::uint32_t count, timing::Micros spin);

    DomainWorkers(const DomainWorkers&) = delete;
    DomainWorkers(DomainWorkers&&) = delete;

    // Stops and joins. A worker inside a region acquire leaves when that acquire ends, which is what
    // cme's own acquire deadline bounds.
    ~DomainWorkers() noexcept;

    // ── operator= ──────────────────────────────────────────────────
    DomainWorkers& operator=(const DomainWorkers&) = delete;
    DomainWorkers& operator=(DomainWorkers&&) = delete;

    // ── public methods ─────────────────────────────────────────────
    // Hands @domainId to an idle worker. False when that domain is already with one, or when none is
    // idle. Either way it is the dispatcher's cue to leave the bit up rather than to wait.
    [[nodiscard]] bool assign(std::uint32_t domainId);

    // ── accessors ──────────────────────────────────────────────────
    // Whether a worker holds @domainId right now. The sweep asks before it touches a turn, because the
    // worker may be about to store the guard it would drop.
    [[nodiscard]] bool isBusy(std::uint32_t domainId) const noexcept;

    // The same fact for every domain at once, for the drain: taking a bit it cannot hand out would mean
    // putting it back, and the dispatcher would then have to be told when to look again.
    [[nodiscard]] const bitmap::AtomicBits<MaxDomains>& busy() const noexcept
    {
        return handing_.inFlight;
    }

    // One event at a time, for the same reason the domain side answers that way.
    [[nodiscard]] std::uint64_t readCount(observe::WorkerEvent asked) const noexcept
    {
        return observe_.events.read(asked);
    }

private:
    // ── who holds what, and who is free ────────────────────────────────
    // The two words a hand-over turns on, in the order assign() touches them: the domain is claimed
    // first, and only then is a worker taken for it.
    struct
    {
        // One bit per domain. Claimed by the dispatcher and dropped by the worker that finishes.
        bitmap::AtomicBits<MaxDomains> inFlight;

        // One bit per worker: raised by a worker with nothing to do, taken by the dispatcher claiming
        // it. Which bit is taken carries no order, so a worker is not chosen for how recently it finished.
        bitmap::AtomicBits<MostWorkers> idle;
    } handing_;

    // ── what the run is doing ──────────────────────────────────────────
    struct
    {
        // Cleared by the destructor before it bumps each worker's word, which is the order a worker
        // reads the two in.
        std::atomic<bool> running{true};
    } state_;

    // ── what a measurement asks afterwards ─────────────────────────────
    // Nothing here is acted on. Its own group, so a word that steers a hand-over is never one of these.
    struct
    {
        observe::EventCounts<observe::WorkerEvent> events;
    } observe_;

    // ── the pool itself ────────────────────────────────────────────────
    // Last of the groups, so every word above is built before the threads that read them start. Each
    // worker holds its own thread, so there is one array and not an array beside a vector.
    struct
    {
        std::unique_ptr<Worker_t[]> workers;
        std::uint32_t count;
    } pool_;
};

}  // namespace cmed::daemon
