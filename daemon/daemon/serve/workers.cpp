// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/serve/workers.cpp -- see workers.hpp.

#include "daemon/serve/workers.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "cmed/errors.hpp"
#include "common/bitmap.hpp"
#include "common/timing.hpp"
#include "daemon/domain/manager.hpp"
#include "daemon/observe/counters.hpp"
#include "shared/protocol/shared_area.hpp"
#include "shared/util/futex.hpp"

namespace cmed::daemon
{

namespace
{

// Called from the worker array's own initialiser, so a count nobody could serve is refused before it
// is asked for as an allocation size. Answers @count, so the refusal sits where the value is used.
[[nodiscard]] std::uint32_t requireWorkerCount(std::uint32_t count)
{
    if (count == 0)
    {
        throw CmedInvalidArgumentError{"cmed::daemon: the worker count is 0, so no domain would be served"};
    }
    if (count > DomainWorkers::MostWorkers)
    {
        throw CmedInvalidArgumentError{"cmed::daemon: the worker count is past what one idle word holds"};
    }
    return count;
}

}  // namespace

// What one worker is, which is the array the pool keeps. Its own cacheline, so handing work to two
// workers at once does not put the two stores on one line.
struct alignas(CachelineBytes) Worker_t
{
    // The one domain handed to it, taken by the exchange that empties this again.
    std::atomic<std::uint32_t> domainId{protocol::NoDomain};

    // Bumped with every hand-over and by the stop, and the word this worker's sleep keys on.
    std::atomic<std::uint32_t> handed{0};

    // Whether this worker is inside the futex sleep. A hand-over to one that is still spinning or
    // running needs no syscall, and the syscall is what a hand-over otherwise costs the dispatcher.
    std::atomic<std::uint32_t> parked{0};

    // Which bit of the pool's idle word is this worker's, which is the whole of its name.
    std::uint32_t seat{0};

    // The thread that runs it. Empty until the pool starts it, and joined before this is destroyed.
    // Here rather than beside the array, so the words a worker turns on and the thread turning them
    // cannot be built in the wrong order.
    std::thread thread;
};

// One line each, checked rather than trusted: two workers on one line would put two hand-overs there.
static_assert(sizeof(Worker_t) == CachelineBytes, "one worker is one line");

namespace
{

// Everything a worker turns with that is not its own. Copied into each thread rather than reached
// through the pool, so what a worker shares is this list and nothing else.
struct Shared_t
{
    // What a worker's own work is: taking the turn for the domain it was handed. The one thing here
    // that may block, which is why it happens on this thread and not on the dispatcher's.
    DomainManager* domains;

    const std::atomic<bool>& running;

    // The domain it drops when it is done, and the word it offers its seat in when it has nothing.
    bitmap::AtomicBits<MaxDomains>& inFlight;
    bitmap::AtomicBits<DomainWorkers::MostWorkers>& idle;

    timing::Nanos spin;
};

// One worker's whole life: wait on its own word, take what is handed to it, drop the claim. Free
// rather than a member, so what a worker reaches for is the two arguments and nothing else.
void runWorker(Worker_t& worker, const Shared_t& shared)
{
    // A bound on one sleep rather than the only way out. The stop bumps this worker's word too, so this
    // is what covers a wake that raced the wait rather than the ordinary path.
    constexpr auto IdleWait = timing::Millis{50};

    while (shared.running.load(std::memory_order_acquire))
    {
        // Read before the offer, so a hand-over landing between the offer and the wait has moved the
        // word and the wait refuses to start.
        const auto seen = worker.handed.load(std::memory_order_acquire);

        const auto domainId = worker.domainId.exchange(protocol::NoDomain, std::memory_order_acq_rel);
        if (domainId == protocol::NoDomain)
        {
            // Idempotent, so a worker that only timed out offers itself again for nothing rather than
            // having to know whether its bit is still up.
            static_cast<void>(shared.idle.claim(worker.seat));
            static_cast<void>(
                util::waitOnWord(worker.handed, seen, timing::Nanos{IdleWait}, shared.spin, worker.parked));
            continue;
        }

        shared.domains->serveLock(domainId);

        // No knock with it. The dispatcher never took a bit it could not hand out, so there is nothing
        // waiting on this worker's news; what is left in the bitmap is picked up by the next pass.
        static_cast<void>(shared.inFlight.release(domainId));
    }
}

}  // namespace

DomainWorkers::DomainWorkers(DomainManager& domains, std::uint32_t count, timing::Micros spin)
    : pool_{std::make_unique<Worker_t[]>(requireWorkerCount(count)), count}
{
    // Built once and copied into each thread. It holds references, so the copies name the same words
    // this pool joins before it lets them go.
    const Shared_t shared{&domains, state_.running, handing_.inFlight, handing_.idle, timing::Nanos{spin}};

    for (std::uint32_t index = 0; index < count; ++index)
    {
        // Its seat before its own thread, which is enough: a worker reads its own seat and no other's,
        // and starting a thread makes what came before it visible there.
        auto& worker = pool_.workers[index];
        worker.seat = index;
        worker.thread = std::thread{[&worker, shared]
                                    {
                                        runWorker(worker, shared);
                                    }};
    }
}

DomainWorkers::~DomainWorkers() noexcept
{
    // The flag before the words, which is the order a worker reads them in: one that saw the wake sees
    // the flag too. Every worker, because that is where each of them sleeps.
    state_.running.store(false, std::memory_order_release);
    for (std::uint32_t index = 0; index < pool_.count; ++index)
    {
        auto& worker = pool_.workers[index];
        worker.handed.fetch_add(1, std::memory_order_release);
        util::wakeAllWaiters(worker.handed);
    }

    // Every word moved before any join, so a worker is not waited out while another still sleeps.
    for (std::uint32_t index = 0; index < pool_.count; ++index)
    {
        pool_.workers[index].thread.join();
    }
}

bool DomainWorkers::assign(std::uint32_t domainId)
{
    if (domainId >= MaxDomains)
    {
        return true;
    }

    // The domain before the worker, so a second assignment cannot pass the first while it waits to be
    // taken. Claimed by the dispatcher alone, which is why the previous bit reads as another's claim.
    if (!handing_.inFlight.claim(domainId))
    {
        observe_.events.bump(observe::WorkerEvent::DeferredBusy);
        return false;
    }

    const auto seat = handing_.idle.takeLowest();
    if (seat == bitmap::NoIndex)
    {
        observe_.events.bump(observe::WorkerEvent::DeferredNoWorker);
        // Nothing claimed this domain after all, so the claim comes back off. Leaving it on would make
        // the domain unassignable until a worker that never took it cleared it.
        static_cast<void>(handing_.inFlight.release(domainId));
        return false;
    }

    // The domain first, then the word, then the wake: a worker that reads the word must find the domain
    // already there. One waiter on this word, so the wake reaches that worker and no other.
    auto& worker = pool_.workers[seat];
    worker.domainId.store(domainId, std::memory_order_release);
    worker.handed.fetch_add(1, std::memory_order_seq_cst);

    // The bump first, then this read: a worker not yet in the sleep finds the word already moved, and
    // one that parks after this read gets EAGAIN from the kernel's own compare instead of missing the hand-over.
    if (worker.parked.load(std::memory_order_seq_cst) != 0)
    {
        util::wakeAllWaiters(worker.handed);
        observe_.events.bump(observe::WorkerEvent::Woke);
    }
    else
    {
        observe_.events.bump(observe::WorkerEvent::SkippedWake);
    }

    observe_.events.bump(observe::WorkerEvent::Handed);
    return true;
}

bool DomainWorkers::isBusy(std::uint32_t domainId) const noexcept
{
    if (domainId >= MaxDomains)
    {
        return false;
    }
    return handing_.inFlight.has(domainId);
}

}  // namespace cmed::daemon
