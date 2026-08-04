// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_fairness_smoke.cpp -- multi-peer ORDER + REQUEST round-trip check.
//
// N peer threads share one ShmMemory, each doing M acquire/release cycles on one domain.
// Every cycle must complete without throwing, and per-peer totals must stay within the
// fairness bound. Light -- ~1s at N=4 / M=500; the full sweep is test_fairness.cpp.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "test_context.hpp"

namespace
{

// One row of the matrix this case covers.
struct Scenario_t
{
    cme::Strategy strategy;     // successor policy the region is formatted with
    std::uint32_t numPeers;     // contending threads, one cme::Peer each
    std::uint32_t iterPerPeer;  // acquire/release cycles every thread runs
    double bound;               // largest per-peer spread accepted, as a fraction of the mean
};

// Both policies at two sizes. The small pair runs first so a break shows up in the cheap
// scenario before the expensive one spends 2000 cycles per peer reaching the same verdict.
constexpr Scenario_t Scenarios[] = {
    {cme::Strategy::Order, 4, 500, 0.15},
    {cme::Strategy::Request, 4, 500, 0.15},
    {cme::Strategy::Order, 8, 2000, 0.15},
    {cme::Strategy::Request, 8, 2000, 0.15},
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
    std::atomic<std::uint32_t>& atStart;
    std::atomic<std::uint32_t>& atEnd;
    std::vector<WorkerResult_t>& results;
    std::uint32_t numPeers;
    std::uint32_t iterPerPeer;
};

// One peer's whole life: join the start line, warm the caches, acquire M times, wait for the
// others to finish. An exception is recorded rather than thrown on, because a thread that
// escapes its body takes the process down with it.
void runWorker(const WorkerContext_t& shared, std::uint32_t pid)
{
    try
    {
        cme::Peer peer{shared.region, pid, shared.ctx.coherency()};

        // Wait at the start-line so peers contend concurrently.
        shared.atStart.fetch_add(1, std::memory_order_release);
        while (shared.atStart.load(std::memory_order_acquire) < shared.numPeers)
        {
            std::this_thread::yield();
        }

        // Every peer is Active in FAM, but the DRAM member caches refresh only per tick. Warm
        // them so we time steady-state handoff, not the cold-start transient.
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        for (std::uint32_t i = 0; i < shared.iterPerPeer; ++i)
        {
            auto guard = peer.lock(0);
            ++shared.results[pid].acquires;
        }

        // End barrier: an early leaver's None status could route a still-running peer's token
        // to a departed peer.
        shared.atEnd.fetch_add(1, std::memory_order_acq_rel);
        while (shared.atEnd.load(std::memory_order_acquire) < shared.numPeers)
        {
            std::this_thread::yield();
        }
    }
    catch (const std::exception& error)
    {
        shared.results[pid].ok = false;
        shared.results[pid].err = error.what();
    }
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

    std::atomic<std::uint32_t> atStart{0};
    std::atomic<std::uint32_t> atEnd{0};
    std::vector<WorkerResult_t> results(numPeers);
    std::vector<std::thread> threads;
    threads.reserve(numPeers);

    const WorkerContext_t shared{ctx, region, atStart, atEnd, results, numPeers, scenario.iterPerPeer};
    for (std::uint32_t pid = 0; pid < numPeers; ++pid)
    {
        threads.emplace_back(runWorker, std::cref(shared), pid);
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

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

    std::printf("  %s : total=%" PRIu64 " max=%" PRIu64 " min=%" PRIu64
                " mean=%.1f dev=%.3f bound=%.2f\n",
                title.c_str(), total, maxv, minv, mean, dev, scenario.bound);

    ctx.check(allOk, (title + ": every peer completed without exception").c_str());
    ctx.check(total == static_cast<std::uint64_t>(numPeers) * scenario.iterPerPeer,
              (title + ": total acquires == N*M").c_str());
    ctx.check(dev <= scenario.bound, (title + ": fairness bound satisfied").c_str());
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    for (const Scenario_t& scenario : Scenarios)
    {
        runOne(ctx, scenario);
    }
}

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, runBody);
}
