// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/serve/handler.hpp -- the serving half of the daemon, running: the pool a domain's turn is
// taken on, the two passes over it, and the threads that turn them. Building one starts both, so a join
// cannot be forgotten: a std::thread destroyed without one ends the process.
//
// A dispatcher pass reads the doorbell, hands out what is pending, and sleeps on the value it read. That
// order is the hazard: a request arriving mid-pass moves the word, so the sleep returns at once.

#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "common/timing.hpp"
#include "daemon/observe/counters.hpp"
#include "daemon/serve/workers.hpp"
#include "daemon/startup/service_notifier.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::daemon
{

// Named and not included: the passes call into these, and the passes are in the .cpp.
struct DaemonConfig_t;
class DomainManager;

class ServeHandler
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // Builds the pool, starts both passes, then announces ready. Built last because readiness claims a
    // bound socket too, which is somebody else's to have made by now. @area and @domains outlive this.
    ServeHandler(protocol::SharedArea_t& area, DomainManager& domains, const DaemonConfig_t& config);

    ServeHandler(const ServeHandler&) = delete;
    ServeHandler(ServeHandler&&) = delete;

    // Stops both passes and joins. Not left to the caller, because a throw between the start and the
    // stop would otherwise reach a destructor with two threads still running.
    ~ServeHandler() noexcept;

    // ── operator= ──────────────────────────────────────────────────
    ServeHandler& operator=(const ServeHandler&) = delete;
    ServeHandler& operator=(ServeHandler&&) = delete;

    // ── accessors ──────────────────────────────────────────────────
    // One event at a time, the way the pool answers it: a run is judged by how the counts move against
    // each other rather than by any one of them being consistent with the rest at an instant.
    [[nodiscard]] std::uint64_t readCount(observe::WorkerEvent asked) const noexcept
    {
        return workers_.readCount(asked);
    }

    // The dispatcher pass's own, which no hand-over count can stand in for: a pass makes several
    // hand-overs or none, so neither number is the other.
    [[nodiscard]] std::uint64_t readCount(observe::DispatchEvent asked) const noexcept
    {
        return dispatching_.events.read(asked);
    }

    // The maintenance pass's own, which no DomainEvent count can stand in for: a pass that found
    // nothing to sweep changes none of those and still ran.
    [[nodiscard]] std::uint64_t readCount(observe::MaintainEvent asked) const noexcept
    {
        return maintaining_.events.read(asked);
    }

private:
    // ── the passes ─────────────────────────────────────────────────
    // Turned by the two threads below and reached by nothing else, so building this object is the whole
    // of starting to serve and destroying it is the whole of stopping.

    // Takes passes until stop(), each domain with work going to a worker. Nothing here may throw: a
    // thread function that lets an exception escape ends the process.
    void runDispatcher();

    // Work no requester rings for, on its own thread since a pass reads the region and would otherwise
    // delay every request that arrived meanwhile. The watchdog ping rides here too.
    void runMaintainer();

    // Ends both. Clears the flag first and knocks second, which is the order a pass reads them in and
    // what keeps the exit prompt.
    void stop() noexcept;

    // ── what both passes hand a domain to ──────────────────────────
    // First, so it is built before the groups below and joined after the passes that reach it. The
    // dispatcher hands a domain to one of these; the maintenance pass asks whether one already holds it.
    DomainWorkers workers_;

    // ── what a dispatcher pass turns with ──────────────────────────
    struct
    {
        // The doorbell it reads, the pending bits it takes, and the word stop() knocks.
        protocol::SharedArea_t* area;

        // How long a pass with nothing to serve waits, and how much of that it spends reading the
        // word before it sleeps on it.
        timing::Millis idleInterval;
        timing::Nanos spin;

        // Nothing here is acted on. Beside the words above rather than in a group of its own, since a
        // pass and how it ended are what these count and both are this group's.
        observe::EventCounts<observe::DispatchEvent> events;
    } dispatching_;

    // ── what a maintenance pass turns with ─────────────────────────
    struct
    {
        // What one pass runs: the registry refresh and the expired-turn sweep.
        DomainManager* domains;

        // Its own pace, which no arrival cuts short.
        timing::Millis interval;

        // Its own doorbell, rung only by stop(). The requesters' doorbell keeps one waiter, so a word
        // shared with it would wake the thread that wants no request.
        std::atomic<std::uint32_t> doorbell{0};

        // Told the dispatcher is still moving, and told nothing when no manager armed a watchdog.
        ServiceNotifier notifier;

        // Nothing here is acted on. Beside the words above rather than in a group of its own, since a
        // pass and its ping are what these count and both are this group's.
        observe::EventCounts<observe::MaintainEvent> events;
    } maintaining_;

    // ── what both passes reach ─────────────────────────────────────
    struct
    {
        // Read at the top of each pass by both, and cleared by stop().
        std::atomic<bool> running{true};

        // Raised once per dispatcher pass and consumed by the maintenance pass, which is the whole of
        // what tells a watchdog the dispatcher is still moving. Nothing else steers on it.
        std::atomic<std::uint64_t> heartbeat{0};

        // The beat the last ping went out on. Not atomic: the maintenance thread is the only one that
        // reads or writes it, which is what makes takeBeat() safe to consume.
        std::uint64_t lastHeartbeat{0};

        // ── the two halves of one liveness answer ──────────────────
        // Raised on the dispatcher's own thread, once per pass it takes.
        void beat() noexcept
        {
            heartbeat.fetch_add(1, std::memory_order_release);
        }

        // Whether the beat moved since this last asked. Consumed by the asking, so the maintenance
        // thread is the only caller: a second one would read the first one's movement as none.
        [[nodiscard]] bool takeBeat() noexcept
        {
            const auto beaten = heartbeat.load(std::memory_order_acquire);
            if (beaten == lastHeartbeat)
            {
                return false;
            }

            lastHeartbeat = beaten;
            return true;
        }
    } common_;

    // ── the threads that turn the passes ───────────────────────────
    // Last, so both start on words already built. Joined by the destructor above.
    std::thread dispatcher_;
    std::thread maintainer_;
};

}  // namespace cmed::daemon
