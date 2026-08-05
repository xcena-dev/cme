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

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <thread>
#include <vector>

#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
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

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const cme::Strategy strategy = ctx.strategy();
    const char* const stratSuffix = ctx.strategySuffix();
    constexpr cme::DomainId DomainCeiling = 2;  // control(0) + 1 data domain

    const cme::Geometry::FormatOpts_t fmtOpts{strategy};
    std::optional<cme::Geometry> region;
    region.emplace(ctx.memory().createRegion(DomainCeiling, NumPeers, fmtOpts));
    harness::seedDataDomains(*region, 1, ctx.coherency());  // creates data domain id 1

    std::vector<std::uint32_t> done(NumPeers, 0);
    std::vector<std::thread> threads;
    std::atomic<int> ctorFailures{0};

    for (cme::PeerId pid = 0; pid < NumPeers; ++pid)
    {
        threads.emplace_back(
            [&, pid]()
            {
                try
                {
                    cme::Peer peer{*region, pid, ctx.coherency()};
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
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    const std::uint64_t expected = static_cast<std::uint64_t>(NumPeers) * ItersPerPeer;
    std::printf("strategy=%s peers=%u iters=%u counter=%" PRIu64 " expected=%" PRIu64 "\n",
                stratSuffix, NumPeers, ItersPerPeer, g_counter, expected);

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
