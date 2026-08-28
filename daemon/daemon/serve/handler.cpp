// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/serve/handler.cpp -- see handler.hpp.

#include "daemon/serve/handler.hpp"

#include <atomic>
#include <cstdint>

#include "common/bitmap.hpp"
#include "common/timing.hpp"
#include "daemon/domain/manager.hpp"
#include "daemon/observe/counters.hpp"
#include "daemon/serve/workers.hpp"
#include "daemon/startup/config.hpp"
#include "daemon/startup/service_notifier.hpp"
#include "shared/protocol/shared_area.hpp"
#include "shared/util/futex.hpp"

namespace cmed::daemon
{

// ── ctor / dtor ────────────────────────────────────────────────────

ServeHandler::ServeHandler(protocol::SharedArea_t& area, DomainManager& domains, const DaemonConfig_t& config)
    : workers_{domains, config.workers.count, config.workers.spin},
      dispatching_{&area, config.serve.idleInterval, timing::Nanos{config.serve.spin}},
      maintaining_{&domains, config.maintenance.interval},
      dispatcher_{[this]
                  {
                      runDispatcher();
                  }},
      maintainer_{[this]
                  {
                      runMaintainer();
                  }}
{
    // Last, and after both threads exist: what this claims is a daemon that can answer, not one that
    // is about to be able to.
    maintaining_.notifier.notifyReady(config.statusLine());
}

ServeHandler::~ServeHandler() noexcept
{
    stop();
    dispatcher_.join();
    maintainer_.join();
}

// ── the loops ──────────────────────────────────────────────────────

// One loop and no helper because getting the order wrong is the whole hazard: the doorbell is read
// before the bits are taken, so a request arriving mid-pass moves the word and the wait returns at once.
void ServeHandler::runDispatcher()
{
    auto& waiting = dispatching_.area->domain.pending;

    while (common_.running.load(std::memory_order_acquire))
    {
        common_.beat();
        dispatching_.events.bump(observe::DispatchEvent::Passes);

        // Read before the taking, not after. A request arriving while this pass runs bumps this word,
        // so the wait below compares against a stale value and returns without sleeping.
        const auto seen = dispatching_.area->readDoorbell();

        // Only the bits no worker holds, and only those are cleared. Taking one that cannot be handed
        // out would mean putting it back, and something would then have to say when to look again.
        const auto held = workers_.busy().takeSnapshot();
        auto taking = waiting.takeSnapshot();

        // What is left up because a worker already holds it, and what a refused hand-over puts back
        // below. Nothing announces a worker finishing, so a pass that leaves any comes back on its own
        // rather than sleeping: a pass that slept would serve it a whole idle turn after it came free.
        bool deferred = !(taking & held).isEmpty();
        taking.dropAll(held);

        bool handed = false;
        if (!taking.isEmpty())
        {
            // Only what this pass will serve. A bit raised while the pass runs is not in the snapshot,
            // so it stays up for the next one rather than being dropped by a whole-word clear.
            waiting.dropAll(taking);

            while (!taking.isEmpty())
            {
                const auto domainId = taking.popLowest();
                if (workers_.assign(domainId))
                {
                    handed = true;
                    continue;
                }

                // No worker free for it. The bit goes back up and no knock goes with it: a worker looks
                // at the queue again when it is done, and a knock here would wake this very pass.
                static_cast<void>(waiting.claim(domainId));
                deferred = true;
            }
        }

        // Something moved, so look again at once rather than wait on a doorbell this pass already read.
        if (handed)
        {
            dispatching_.events.bump(observe::DispatchEvent::Handed);
            continue;
        }

        // The flag clears before the knock, so a read that saw the knock sees the flag too and does not
        // sleep out a timeout on the very knock meant to end it.
        if (!common_.running.load(std::memory_order_acquire))
        {
            return;
        }

        // One spin window and no sleep when a domain is with a worker: what frees it is that worker
        // finishing, which moves no word this could wait on.
        dispatching_.events.bump(deferred ? observe::DispatchEvent::Deferred : observe::DispatchEvent::Idle);
        const auto waitFor = deferred ? dispatching_.spin : timing::Nanos{dispatching_.idleInterval};
        static_cast<void>(util::waitOnWord(dispatching_.area->daemon.doorbell, seen, waitFor, dispatching_.spin,
                                           dispatching_.area->daemon.dispatcherParked));
    }
}

// The sweep, and the watchdog ping behind it. The ping rides this thread because the thread proves
// nothing on its own: it goes out only when the dispatcher's pass count moved since the last tick, so a
// dispatcher that stopped taking passes stops the pings while this thread keeps ticking.
void ServeHandler::runMaintainer()
{
    while (common_.running.load(std::memory_order_acquire))
    {
        // On the thread that never blocks. A turn whose hold ran out has to go back even while a worker
        // waits on an acquire for a different domain.
        maintaining_.domains->maintain([this](std::uint32_t domainId)
                                       {
                                           return workers_.isBusy(domainId);
                                       });

        maintaining_.events.bump(observe::MaintainEvent::Passes);

        // The dispatcher's beat and not this thread's, so a dispatcher that stopped taking passes is
        // what stops the pings while this thread keeps ticking.
        if (common_.takeBeat())
        {
            maintaining_.notifier.notifyAlive();
            maintaining_.events.bump(observe::MaintainEvent::Alive);
        }
        else
        {
            maintaining_.events.bump(observe::MaintainEvent::Silent);
        }

        // No spin: this thread has nothing to catch early. Its own doorbell rather than the requesters',
        // so nothing but the stop cuts a pass short.
        const auto seen = maintaining_.doorbell.load(std::memory_order_acquire);
        static_cast<void>(
            util::waitOnWord(maintaining_.doorbell, seen, timing::Nanos{maintaining_.interval},
                             timing::Nanos::zero()));
    }
}

// ── ending them ────────────────────────────────────────────────────

void ServeHandler::stop() noexcept
{
    common_.running.store(false, std::memory_order_release);

    // The knock and not only a wake, because a pass passes the doorbell it read to the kernel. A wake
    // alone lands on a queue this thread may not have joined yet, and is then lost.
    dispatching_.area->knock();

    // And the maintainer's own doorbell, which no requester rings.
    maintaining_.doorbell.fetch_add(1, std::memory_order_release);
    util::wakeAllWaiters(maintaining_.doorbell);
}

}  // namespace cmed::daemon
