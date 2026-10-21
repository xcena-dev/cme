// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_fairness_smoke.cpp -- multi-peer ORDER + REQUEST round-trip check.
//
// N peer threads share one ShmMemory, each doing M acquire/release cycles on one domain.
// A liveness check: every cycle must complete without throwing and every peer must finish all
// M of them. Light by design; the full parameter sweep lives in test_fairness.cpp.
//
// The per-peer spread is printed, not gated. Every peer gets the same fixed M, so the spread is
// zero whenever those two hold, and starvation surfaces through lock()'s own deadline instead.

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cme/shared.hpp"
#include "common/ordered.hpp"
#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

// One row of the matrix this case covers.
struct Scenario_t
{
    cme::Strategy strategy;     // successor policy the region is formatted with
    std::uint32_t numPeers;     // contending threads, one cme::Peer each
    std::uint32_t iterPerPeer;  // acquire/release cycles every thread runs
};

// Both policies at two sizes. The small pair runs first so a break shows up in the cheap
// scenario before the expensive one spends 2000 cycles per peer reaching the same verdict.
constexpr Scenario_t Scenarios[] = {
    {cme::Strategy::Order, 4, 500},
    {cme::Strategy::Request, 4, 500},
    {cme::Strategy::Order, 8, 2000},
    {cme::Strategy::Request, 8, 2000},
};

struct WorkerResult_t
{
    std::uint64_t acquires{0};
    bool ok{true};
    std::string err{};
};

// Everything a worker thread reads, gathered so the thread body can be a named function.
// Seven parameters would read worse than one, and every member outlives the threads: the
// run that owns them joins before it returns.
struct WorkerContext_t
{
    harness::TestContext& ctx;
    cme::Geometry& region;
    ordered::Published<std::uint32_t>& atStart;
    ordered::Published<std::uint32_t>& atEnd;
    std::vector<WorkerResult_t>& results;
    std::uint32_t numPeers;
    std::uint32_t iterPerPeer;
};

// Counts this peer in at one line, then waits until every peer has arrived.
void arriveAndWait(ordered::Published<std::uint32_t>& line, std::uint32_t numPeers)
{
    line.add(1);
    while (line.load() < numPeers)
    {
        std::this_thread::yield();
    }
}

// One peer's whole life: join the start line, warm the caches, acquire M times, wait for the
// others to finish. An exception is recorded rather than thrown on, because a thread that
// escapes its body takes the process down with it.
void runWorker(const WorkerContext_t& shared, std::uint32_t pid)
{
    WorkerResult_t& result = shared.results[pid];
    const auto recordFailure = [&result](const std::exception& error)
    {
        result.ok = false;
        result.err = error.what();
    };

    // A failed peer still passes both lines. Skipping one leaves the rest spinning there until
    // ctest's timeout, and the recorded failure never prints.
    std::optional<cme::Peer> peer;
    try
    {
        peer.emplace(shared.region, pid);
    }
    catch (const std::exception& error)
    {
        recordFailure(error);
    }

    // Wait at the start-line so peers contend concurrently.
    arriveAndWait(shared.atStart, shared.numPeers);
    if (peer)
    {
        try
        {
            // Every peer is Active in FAM, but the DRAM member caches refresh only per tick. Warm
            // them so we time steady-state handoff, not the cold-start transient.
            std::this_thread::sleep_for(timing::Millis{20});
            for (std::uint32_t cycle = 0; cycle < shared.iterPerPeer; ++cycle)
            {
                auto guard = peer->lock(0);
                ++result.acquires;
            }
        }
        catch (const std::exception& error)
        {
            recordFailure(error);
        }
    }

    // End barrier: an early leaver's None status could route a still-running peer's token
    // to a departed peer.
    arriveAndWait(shared.atEnd, shared.numPeers);
}

void runOne(harness::TestContext& ctx, const Scenario_t& scenario)
{
    const std::uint32_t numPeers = scenario.numPeers;
    const std::string title = (scenario.strategy == cme::Strategy::Order ? "ORDER" : "REQUEST") +
                              std::string{" n="} + std::to_string(numPeers) +
                              " i=" + std::to_string(scenario.iterPerPeer);

    // Every scenario shares the case's one region, so clear it before each format.
    ctx.memory().remove();
    auto region =
        ctx.memory().createRegion(1, numPeers, cme::Geometry::FormatOpts_t{scenario.strategy});

    ordered::Published<std::uint32_t> atStart{0};
    ordered::Published<std::uint32_t> atEnd{0};
    std::vector<WorkerResult_t> results(numPeers);

    const WorkerContext_t shared{ctx, region, atStart, atEnd, results, numPeers, scenario.iterPerPeer};
    harness::runThreads(numPeers,
                        [&shared](std::uint32_t pid)
                        {
                            runWorker(shared, pid);
                        });

    std::uint64_t total = 0;
    std::uint64_t maxv = 0;
    std::uint64_t minv = UINT64_MAX;
    bool allOk = true;
    for (std::uint32_t pid = 0; pid < numPeers; ++pid)
    {
        const auto& result = results[pid];
        if (!result.ok)
        {
            std::printf("  peer %u: %s\n", pid, result.err.c_str());
            allOk = false;
        }
        total += result.acquires;
        maxv = std::max(maxv, result.acquires);
        minv = std::min(minv, result.acquires);
    }
    const auto mean = static_cast<double>(total) / numPeers;
    const auto dev = mean > 0 ? static_cast<double>(maxv - minv) / mean : 0.0;

    std::printf("  %s : total=%" PRIu64 " max=%" PRIu64 " min=%" PRIu64 " mean=%.1f dev=%.3f\n",
                title.c_str(), total, maxv, minv, mean, dev);

    ctx.check(allOk, (title + ": every peer completed without exception").c_str());
    ctx.check(total == static_cast<std::uint64_t>(numPeers) * scenario.iterPerPeer,
              (title + ": total acquires == N*M").c_str());
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    for (const Scenario_t& scenario : Scenarios)
    {
        runOne(ctx, scenario);
    }
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
