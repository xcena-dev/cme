// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// recovery_latency_bench.cpp -- §11.5 recovery-latency measurement.
//
// Recovery timeline after a permanent crash, broken into phases and swept over the dead
// peer's owned-domain count N. The main thread reads the region directly to timestamp each
// transition, so no event-hook plumbing is needed.
//
// Dead peer D holds all N domains in simultaneous *pinned* guards, so its own poll thread
// cannot grant them away and the survivors block until recovery takes them over.
//
// Phases, relative to the freeze instant t0:
//   detect+claim = t_recovering - t0          (Active -> Recovering; DeadWindow dominates)
//   takeover     = t_seized - t_recovering    (every domain's holder != D)
//   finish       = t_none - t_seized          (Recovering -> None)
// Also per-domain takeover cost and survivor time-to-resume.
//
// backend: --backend shm|dax|uc, resolved against config.yaml.
// options: --strategy, --domains (a single count, else the swept set), --peers (default 6).

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/args.hpp"
#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace
{

constexpr cme::PeerId DefaultPeers = 6;

// N + the control domain must fit MaxDomains, which is 64.
const std::vector<cme::DomainId> SweptDomainCounts = {1, 4, 16, 32, 63};

constexpr std::uint32_t ReadyPollMs = 1;
constexpr std::uint32_t FrozenIdleMs = 20;
constexpr std::uint32_t SteadyStateMs = 200;
constexpr std::uint32_t ResumeWindowMs = 300;
constexpr double TimeoutMs = 15000.0;

// The shared slot plus what this measurement needs. The base's `peer` field goes unused: the loop
// below keeps its Peer on the stack, which is stronger than a slot field since nothing outside the
// thread can even name it.
struct BenchSlot_t : harness::PeerSlot_t
{
    bool isDead{false};              // this peer is the one that crashes while holding every domain
    std::atomic<bool> ready{false};  // the peer has joined and, if dead, holds all its guards
};

// One peer thread. The dead peer locks all N domains into simultaneous pinned guards, publishes
// ready, then freezes while holding them (permanent crash). Survivors loop lock/unlock over the N
// domains, counting each acquire.
void worker(BenchSlot_t* slot)
{
    try
    {
        auto peer = std::make_unique<cme::Peer>(*slot->region, slot->peerId, slot->coherency);
        for (cme::DomainId domainId = 1; domainId <= slot->domainCount; ++domainId)
        {
            peer->joinDomain(domainId);
        }

        if (slot->isDead)
        {
            // Hold all N domains at once (pinned guards), then freeze while holding.
            std::vector<cme::PeerGuard> held;
            held.reserve(slot->domainCount);
            for (cme::DomainId domainId = 1; domainId <= slot->domainCount; ++domainId)
            {
                held.push_back(peer->lock(domainId));
            }
            slot->ready.store(true, std::memory_order_release);
            while (!slot->frozen.load(std::memory_order_acquire) && !slot->stop.load())
            {
                std::this_thread::sleep_for(timing::Millis{ReadyPollMs});
            }
            peer->setFreeze(true);  // permanent crash: stop the poll thread, keep the pins
            while (!slot->stop.load())
            {
                std::this_thread::sleep_for(timing::Millis{FrozenIdleMs});
            }
            return;  // never release the guards (crash model); leak intentionally on stop
        }

        slot->ready.store(true, std::memory_order_release);
        cme::DomainId domainId = 1;
        while (!slot->stop.load(std::memory_order_acquire))
        {
            try
            {
                const cme::PeerGuard guard = peer->lock(domainId);
                slot->acquires.fetch_add(1, std::memory_order_relaxed);
            }
            catch (const cme::LockTimeoutError&)
            {
                // @expected a lock attempt that times out under contention is a normal outcome, not a failure: the loop moves on to the next domain.
            }
            domainId = (domainId % slot->domainCount) + 1;
        }
        peer.reset();
    }
    catch (const std::exception& error)
    {
        std::printf("peer %u worker exception: %s\n", slot->peerId, error.what());
    }
}

struct Phases_t
{
    double detectClaimMs{0};
    double takeoverMs{0};
    double finishMs{0};
    double totalMs{0};
    std::uint64_t survivorResumeAcq{0};
    bool timedOut{false};
};

// Reads on the shared region (main thread; cme::coherency::get = rmb + whole-slot load).
[[nodiscard]] bool isRecovering(cme::Geometry& region, cme::PeerId peerId,
                                cme::CoherencyMode coherency)
{
    return cme::coherency::get(region.getMemberSlot(peerId), coherency).isRecovering();
}

[[nodiscard]] bool isNone(cme::Geometry& region, cme::PeerId peerId, cme::CoherencyMode coherency)
{
    return cme::coherency::get(region.getMemberSlot(peerId), coherency)
        .hasStatus(cme::Geometry::Member_t::Status::None);
}

[[nodiscard]] bool allSeized(cme::Geometry& region, cme::PeerId peerId, cme::DomainId domains,
                             cme::CoherencyMode coherency)
{
    for (cme::DomainId domainId = 1; domainId <= domains; ++domainId)
    {
        if (cme::coherency::get(region.getDomainRecord(domainId), coherency).getHolder() ==
            peerId)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Phases_t runOne(cme::Strategy strategy, cme::DomainId domains, cme::PeerId peers,
                              const std::string& uri, cme::CoherencyMode coherency)
{
    // Dead peer = 0: on join it re-adopts the genesis ownership parked on slot 0,
    // so it holds all N domains immediately (resident fast path) without needing a
    // grantor -- no other peer is up yet. Survivors are 1..peers-1.
    const cme::PeerId deadPeer = 0;

    auto region =
        cme::Geometry::create(uri, domains + 1, peers, cme::Geometry::FormatOpts_t{strategy});
    harness::seedDataDomains(region, domains);  // create data domains 1..n

    std::vector<std::unique_ptr<BenchSlot_t>> slots;
    for (cme::PeerId peerId = 0; peerId < peers; ++peerId)
    {
        auto slot = std::make_unique<BenchSlot_t>();
        slot->region = &region;
        slot->peerId = peerId;
        slot->domainCount = domains;
        slot->coherency = coherency;
        slot->isDead = (peerId == deadPeer);
        slots.push_back(std::move(slot));
    }

    // Dead peer first: it must hold all N (pinned) before survivors contend, so its
    // poll thread never grants a held domain away.
    slots[deadPeer]->runner = std::thread{worker, slots[deadPeer].get()};
    while (!slots[deadPeer]->ready.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(timing::Millis{ReadyPollMs});
    }
    for (std::unique_ptr<BenchSlot_t>& slot : slots)
    {
        if (!slot->isDead)
        {
            slot->runner = std::thread{worker, slot.get()};
        }
    }
    for (std::unique_ptr<BenchSlot_t>& slot : slots)
    {
        while (!slot->ready.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(timing::Millis{ReadyPollMs});
        }
    }
    // Steady state: every survivor is blocked on a domain the dead peer pins.
    std::this_thread::sleep_for(timing::Millis{SteadyStateMs});

    std::uint64_t acquiredBefore = 0;
    for (std::unique_ptr<BenchSlot_t>& slot : slots)
    {
        if (!slot->isDead)
        {
            acquiredBefore += slot->acquires.load();
        }
    }

    // One instant, asked two questions: how long each phase took, and whether the whole thing has
    // run out of time. Neither class answers both, so both start here.
    const timing::Stopwatch sinceFreeze;
    const timing::Deadline budget{timing::MillisF{TimeoutMs}};
    slots[deadPeer]->frozen.store(true, std::memory_order_release);  // start the clock

    Phases_t phases;
    // Busy-spin every wait: the DeadWindow (detect) is ~O(100ms) but claim/takeover/
    // finish are microseconds apart, so a sleeping poll would collapse them to 0.
    while (!isRecovering(region, deadPeer, coherency) &&
           !isNone(region, deadPeer, coherency) &&
           !budget.expired())
    {
    }
    phases.detectClaimMs = sinceFreeze.elapsed<timing::MillisF>().count();
    while (!allSeized(region, deadPeer, domains, coherency) && !budget.expired())
    {
    }
    const double seizedAtMs = sinceFreeze.elapsed<timing::MillisF>().count();
    phases.takeoverMs = seizedAtMs - phases.detectClaimMs;
    while (!isNone(region, deadPeer, coherency) && !budget.expired())
    {
    }
    phases.totalMs = sinceFreeze.elapsed<timing::MillisF>().count();
    phases.finishMs = phases.totalMs - seizedAtMs;
    phases.timedOut = phases.totalMs >= TimeoutMs;

    // Survivor liveness: acquisitions landed in a fixed window AFTER recovery
    // finishes (they were fully stalled for `total` before this).
    std::this_thread::sleep_for(timing::Millis{ResumeWindowMs});
    std::uint64_t acquiredAfter = 0;
    for (std::unique_ptr<BenchSlot_t>& slot : slots)
    {
        if (!slot->isDead)
        {
            acquiredAfter += slot->acquires.load();
        }
    }
    phases.survivorResumeAcq = acquiredAfter - acquiredBefore;

    for (std::unique_ptr<BenchSlot_t>& slot : slots)
    {
        slot->stop.store(true);
    }
    for (std::unique_ptr<BenchSlot_t>& slot : slots)
    {
        if (slot->runner.joinable())
        {
            slot->runner.join();
        }
    }
    return phases;
}

}  // namespace

// Every option here comes through argStr/argU64, which read what the harness stored, so
// this takes no argv of its own.
void runBench(harness::TestContext& ctx)
{
    const cme::Strategy strategy = ctx.strategy();
    const char* const stratSuffix = ctx.strategySuffix();
    const auto peers = static_cast<cme::PeerId>(cliargs::argU64("--peers", DefaultPeers));

    // One region per swept point on shm and uc; on dax the device hands out one window per
    // --slot, so the points reuse it in turn.
    auto uriFor = [&](cme::DomainId domains)
    {
        return ctx.memory().uriFor(std::to_string(domains));
    };

    std::vector<cme::DomainId> domainCounts;
    if (const std::uint64_t single = cliargs::argU64("--domains", 0); single != 0)
    {
        domainCounts = {static_cast<cme::DomainId>(single)};
    }
    else
    {
        domainCounts = SweptDomainCounts;
    }

    std::printf("# recovery latency  strategy=%s peers=%u  (ms)\n", stratSuffix, peers);
    std::printf("%-4s %-13s %-10s %-9s %-9s %-13s %-8s\n",
                "N", "detect+claim", "takeover", "finish", "total", "perDomTk", "resumeAcq");

    bool anyTimedOut = false;
    for (const cme::DomainId domains : domainCounts)
    {
        const Phases_t phases =
            runOne(strategy, domains, peers, uriFor(domains), ctx.coherency());
        anyTimedOut = anyTimedOut || phases.timedOut;
        std::printf("%-4u %-13.2f %-10.2f %-9.2f %-9.2f %-13.3f %-8" PRIu64 "%s\n", domains,
                    phases.detectClaimMs, phases.takeoverMs, phases.finishMs, phases.totalMs,
                    domains ? phases.takeoverMs / domains : 0.0, phases.survivorResumeAcq,
                    phases.timedOut ? "  TIMEOUT" : "");
    }

    // A row that timed out is a recovery that did not finish, which is a failure to
    // complete rather than a slow measurement. Reporting it green would let a gate that
    // runs this call recovery healthy while it is broken.
    ctx.check(!anyTimedOut, "every swept domain count finished recovery inside the deadline");
}

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, runBench);
}
