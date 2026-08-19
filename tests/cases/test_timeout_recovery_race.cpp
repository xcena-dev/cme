// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_timeout_recovery_race.cpp -- waiting on a domain whose holder dies mid-wait.
//
// timeout_late_grant races a live holder's release against the deadline. Here the holder dies
// holding, so the domain arrives only once recovery seizes it and the deadline expires on the way.
// The failure ruled out is a waiter that gives up while keeping the belief, which would let the
// next acquire return instantly on a view recovery has already overwritten.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

constexpr cme::PeerId MaxPeers = 4;
constexpr cme::DomainId NumDomains = 1;
constexpr cme::DomainId Ceiling = NumDomains + 1;  // control + data
constexpr cme::DomainId Contested = 1;
constexpr cme::PeerId Waiter = MaxPeers - 1;  // main's own peer, outside the worker group
constexpr cme::PeerId Holder = 0;             // the worker pinned to hold the domain and die on it

// The pinned worker takes the domain on its first pass, so this only bounds a run where it never
// got it at all.
constexpr std::uint32_t HoldDeadlineMs = 5000;

constexpr std::uint32_t RecoveryDeadlineMs = 20000;

// Short enough that the wait expires many times inside the liveness grace window, which is where
// the give-up and the seizure have to be kept apart.
constexpr std::uint32_t AttemptMs = 200;
constexpr std::uint32_t Attempts = 60;

}  // namespace

void runBody(harness::TestContext& ctx)
{
    auto region = harness::createRegion(Ceiling, MaxPeers);
    harness::seedDataDomains(region, NumDomains);

    std::printf("timeout recovery race: %u attempts of %u ms (%s, backend=%s)\n", Attempts,
                AttemptMs, ctx.strategySuffix(), ctx.backendName());

    // Peers 0..Waiter-1 work the domain. Holder is pinned, so it takes the domain and keeps it:
    // the freeze below then lands on a peer that is provably holding, rather than on whichever one
    // a single record sample happened to name.
    std::array<harness::PeerSlot_t, Waiter> workers{};
    workers[Holder].pinned.store(true);
    harness::spawnPeerWorkers(workers, Waiter, region, NumDomains);
    harness::sleepMs(1000);
    if (!ctx.check(harness::allPeersJoined(workers, Waiter), "every worker joined the domain"))
    {
        harness::joinPeerWorkers(workers, Waiter);
        return;
    }

    auto waiter = harness::makePeer(region, Waiter);
    waiter.joinDomain(Contested);

    // Freeze the holder only once its hold is real, so the domain dies held rather than idle.
    const bool held = harness::waitUntil(
        [&workers]
        {
            return workers[Holder].holding.load(std::memory_order_acquire);
        },
        HoldDeadlineMs, 1);
    if (!ctx.check(held, "a worker held the domain when the kill landed"))
    {
        harness::joinPeerWorkers(workers, Waiter);
        return;
    }
    workers[Holder].frozen.store(true);
    workers[Holder].abandon.store(true);

    // Hammer the domain across the whole grace-plus-recovery window. Every attempt that fails must
    // fail cleanly: no guard, and the record not naming the waiter.
    bool validOnArrival = false;
    bool acquired = false;
    std::uint32_t failures = 0;
    for (std::uint32_t attempt = 0; attempt < Attempts && !acquired; ++attempt)
    {
        auto guard = waiter.tryLock(Contested, timing::Millis{AttemptMs});
        if (guard)
        {
            acquired = true;
            // Read the record here, not after the failures: naming a peer that asked for nothing is
            // order and agg working. What must not happen is this guard resting on a stale belief.
            validOnArrival = harness::readDomainRecord(region, Contested).holder == Waiter;
            guard.reset();
            break;
        }
        ++failures;
    }

    std::printf("peer %u died holding; the waiter failed %u times before it got in\n",
                Holder, failures);

    ctx.check(failures > 0, "the dead holder made the waiter miss at least once");
    ctx.check(acquired, "the domain arrived once recovery seized it");
    ctx.check(validOnArrival, "the guard that arrived named the waiter in the record");
    ctx.check(
        harness::waitUntil(
            [&region]
            {
                return harness::hasMemberStatus(region, Holder, Status::None);
            },
            RecoveryDeadlineMs),
        "and the dead holder's slot was finished");

    harness::joinPeerWorkers(workers, Waiter);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
