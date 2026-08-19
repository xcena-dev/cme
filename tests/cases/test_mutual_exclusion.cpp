// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_mutual_exclusion.cpp -- the core contract: at most one holder per domain.
//
// N peers hammer ONE domain, each doing a non-atomic RMW on a shared counter inside the CS.
// Exclusive ownership means the increments serialise to exactly N*M; any concurrent holder
// loses an update and the total falls short. Also asserts bounded-wait.
//
// Strategy from --strategy, registered at high peer count for peterson's deep tournament
// tree. Peers are threads sharing one region, each with its own cme::Peer.

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr cme::PeerId NumPeers = 16;  // power-of-two-ish; deep peterson tree
constexpr std::uint32_t ItersPerPeer = 2000;
constexpr cme::DomainId SharedDomain = 1;  // the one contended data domain

// Non-atomic on purpose: the lock is what must serialize the RMW. A torn/lost
// increment (two holders at once) shows up as final < NumPeers*ItersPerPeer.
std::uint64_t g_counter = 0;

// What the prober is willing to wait, and how long the pinned worker gets to take the domain in the
// first place. The hold itself is unbounded: it lasts until joinPeerWorkers stops it.
constexpr std::uint32_t ProbeMs = 50;
constexpr std::uint32_t HoldDeadlineMs = 5000;

// The holder is the domain's creator on purpose: a fast path admitting on the genesis-holder record
// alone lets that first acquire skip the lock, and a one-increment critical section cannot see it.
void checkHeldDomainStaysHeld(harness::TestContext& ctx)
{
    constexpr cme::DomainId DomainCeiling = 2;
    constexpr cme::PeerId Peers = 2;
    auto region = harness::createRegion(DomainCeiling, Peers);
    harness::seedDataDomains(region, 1);  // its transient peer 0 is the genesis holder

    std::array<harness::PeerSlot_t, 1> holder{};
    holder[0].pinned.store(true);  // takes the domain and keeps it, rather than churning
    harness::spawnPeerWorkers(holder, 1, region, 1);

    bool intruded = false;
    auto prober = harness::makePeer(region, 1);
    prober.joinDomain(SharedDomain);
    // Asserted, not assumed: without the hold there is nothing to intrude on, and the check below
    // would pass on a run that never set the situation up.
    const bool held = harness::waitUntil(
        [&holder]
        {
            return holder[0].holding.load(std::memory_order_acquire);
        },
        HoldDeadlineMs, 1);
    if (held)
    {
        auto guard = prober.tryLock(SharedDomain, timing::Millis{ProbeMs});
        intruded = guard && holder[0].holding.load(std::memory_order_acquire);
    }
    harness::joinPeerWorkers(holder, 1);

    ctx.check(held, "the creator took the domain and kept it");
    ctx.check(!intruded, "a second peer took the domain while the creator still held it");
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    // First, so its region is gone before the counter run formats its own.
    checkHeldDomainStaysHeld(ctx);

    constexpr cme::DomainId DomainCeiling = 2;  // control(0) + 1 data domain

    auto region = harness::createRegion(DomainCeiling, NumPeers);
    harness::seedDataDomains(region, 1);  // creates data domain id 1

    std::vector<std::uint32_t> done(NumPeers, 0);
    std::atomic<int> ctorFailures{0};

    harness::runThreads(
        NumPeers,
        [&](cme::PeerId pid)
        {
            try
            {
                auto peer = harness::makePeer(region, pid);
                peer.joinDomain(SharedDomain);
                for (std::uint32_t i = 0; i < ItersPerPeer; ++i)
                {
                    auto guard = peer.lock(SharedDomain);  // exclusive
                    // Critical section: non-atomic RMW, serialized only by the lock.
                    ++g_counter;
                    ++done[pid];
                }
            }
            catch (const std::exception& e)
            {
                std::fprintf(stderr, "peer %u: %s\n", pid, e.what());
                ctorFailures.fetch_add(1);
            }
        });

    const std::uint64_t expected = static_cast<std::uint64_t>(NumPeers) * ItersPerPeer;
    std::printf("strategy=%s peers=%u iters=%u counter=%" PRIu64 " expected=%" PRIu64 "\n",
                ctx.strategySuffix(), NumPeers, ItersPerPeer, g_counter, expected);

    ctx.check(ctorFailures.load() == 0, "every peer joined + ran without exception");
    ctx.check(g_counter == expected, "mutual exclusion: no lost update (counter == N*M)");

    bool allDone = true;
    for (cme::PeerId pid = 0; pid < NumPeers; ++pid)
    {
        if (done[pid] != ItersPerPeer)
        {
            allDone = false;
        }
    }
    ctx.check(allDone, "bounded-wait: every peer completed all M acquires (no starvation)");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
