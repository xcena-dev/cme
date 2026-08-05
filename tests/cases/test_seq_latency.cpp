// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_seq_latency.cpp -- uncontended sequential lock-acquire latency bench.
//
// One lock at a time from a single driver loop: pick a random peer and data domain,
// measure peer.lock(domain), release, repeat. No contention or queueing, so the number is
// the pure acquire cost, dominated by the handoff when the domain sits on another peer.
//
// Every peer is live (own poll thread), so a handoff runs the real transfer path. Holder =
// last locker, so a random pick migrates with prob ~(N-1)/N and the rest are resident
// re-locks; we report the split, and --force-migrate makes every pick a migration.
//
// Backend via --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.
// Strategy via --order|--request|--request-agg|--peterson (default request).
//
//   -n N            peers (default 8)
//   -d D            domains incl. control slot 0 (default 8 -> 7 data domains)
//   -i ITER         measured sequential acquires (default 20000)
//   --warmup W      unmeasured warm-up acquires (default 1000)
//   --force-migrate always pick requester != current holder
//   --seed S        RNG seed (default 1)

// getopt_long is attributed to <bits/getopt_ext.h>, which .clang-tidy ignores, so the
// include-cleaner fixer reads this header as unused and deletes it. The NOLINT stops that.
#include <getopt.h>  // NOLINT(misc-include-cleaner)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "observe/latency.hpp"  // trace::writeJsonl (no-op unless CME_LATENCY)
#include "test_context.hpp"
#include "test_options.hpp"
#include "util/time.hpp"

namespace test
{
namespace
{

struct Opts_t
{
    std::uint32_t numPeers{8};
    std::uint32_t numDomains{8};
    std::uint32_t iters{20000};
    std::uint32_t warmup{1000};
    bool forceMigrate{false};
    std::uint64_t seed{1};
    cme::Strategy strategy{cme::Strategy::Request};
    // Provision exactly what the run uses (set post-parse): maxPeers = numPeers so the
    // Peterson tournament tree sizes to roundUpPow2(numPeers) -- climb depth tracks the
    // real peer count instead of a fixed cap. maxDomains = numDomains likewise.
    std::uint32_t maxPeers{0};
    std::uint32_t maxDomains{0};
    const char* csvPath{nullptr};     // --csv: append one sweep row (migrate stats, us)
    const char* traceJsonl{nullptr};  // --trace-jsonl: dump CME_LATENCY span trace here
};

// Sanity ceiling; provisioning tracks numPeers/numDomains, not this.
constexpr std::uint32_t MaxProvision = 4096;

int parseOpts(int argc, char** argv, Opts_t& opt)
{
    static const option Long[] = {
        {"warmup", required_argument, nullptr, 'w'},
        {"force-migrate", no_argument, nullptr, 'm'},
        {"seed", required_argument, nullptr, 's'},
        {"csv", required_argument, nullptr, 'C'},
        {"trace-jsonl", required_argument, nullptr, 'T'},
        {"order", no_argument, nullptr, 1000},
        {"request", no_argument, nullptr, 1001},
        {"request-agg", no_argument, nullptr, 1002},
        {"peterson", no_argument, nullptr, 1003},
        {nullptr, 0, nullptr, 0},
    };
    int flag = 0;
    while ((flag = getopt_long(argc, argv, "n:d:i:w:s:C:T:", Long, nullptr)) != -1)
    {
        switch (flag)
        {
            case 'n':
                opt.numPeers = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 'd':
                opt.numDomains = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 'i':
                opt.iters = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 'w':
                opt.warmup = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 'm':
                opt.forceMigrate = true;
                break;
            case 's':
                opt.seed = static_cast<std::uint64_t>(std::strtoull(optarg, nullptr, 0));
                break;
            case 'C':
                opt.csvPath = optarg;
                break;
            case 'T':
                opt.traceJsonl = optarg;
                break;
            case 1000:
                opt.strategy = cme::Strategy::Order;
                break;
            case 1001:
                opt.strategy = cme::Strategy::Request;
                break;
            case 1002:
                opt.strategy = cme::Strategy::RequestAgg;
                break;
            case 1003:
                opt.strategy = cme::Strategy::Peterson;
                break;
            default:
                return -1;
        }
    }
    if (opt.numDomains < 2)
    {
        std::fprintf(stderr, "need -d >= 2 (slot 0 is control)\n");
        return -1;
    }
    if (opt.numPeers < 1 || opt.numPeers > MaxProvision || opt.numDomains > MaxProvision)
    {
        std::fprintf(stderr, "peers 1..%u, domains 2..%u\n", MaxProvision, MaxProvision);
        return -1;
    }
    // Provision exactly the run size -> Peterson tree = roundUpPow2(numPeers).
    opt.maxPeers = opt.numPeers;
    opt.maxDomains = opt.numDomains;
    return 0;
}

// Every peer is a live instance with its own poll thread (needed to forward/grant
// handoffs). The driver below is the sole locker, so nothing here contends.
std::vector<std::unique_ptr<cme::Peer>> buildPeers(const Opts_t& opt, cme::Geometry& region,
                                                   cme::CoherencyMode coherency)
{
    std::vector<std::unique_ptr<cme::Peer>> peers;
    peers.reserve(opt.numPeers);
    for (cme::PeerId pid = 0; pid < opt.numPeers; ++pid)
    {
        auto peer = std::make_unique<cme::Peer>(region, pid, coherency);
        for (cme::DomainId domainId = 1; domainId < opt.numDomains; ++domainId)
        {
            peer->joinDomain(domainId);
        }
        peers.push_back(std::move(peer));
    }
    return peers;
}

// Hands exactly one lock request at a time to a dedicated acquire thread per peer, so the
// trace splits into per-peer lanes instead of collapsing to one. Store order and memory
// order below are load-bearing -- see spawnAcquireThreads/oneAcquire.
struct Baton_t
{
    std::atomic<int> turn{-1};  // peer id whose turn it is (-1 = idle)
    std::atomic<bool> stopThreads{false};
    std::atomic<bool> turnDone{false};
    std::atomic<std::uint32_t> resultNs{0};
    std::atomic<bool> resultTimeout{false};
};

// This peer's turn: time the acquire, publish the sample, then hand the baton back. The
// Guard's inner scope and the -1-then-turnDone store order are load-bearing: reversing either
// lets the coordinator start the next turn before this peer's release lands.
void serveOneTurn(Baton_t& baton, cme::Peer* self, cme::DomainId domain)
{
    try
    {
        const auto began = std::chrono::steady_clock::now();
        {
            auto guard = self->lock(domain);
            const auto held = std::chrono::steady_clock::now();
            baton.resultNs.store(static_cast<std::uint32_t>(std::min<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(held - began).count(),
                UINT32_MAX)));
            baton.resultTimeout.store(false, std::memory_order_relaxed);
        }  // released here -- domain takeable by the next peer
    }
    catch (const cme::LockTimeoutError&)
    {
        baton.resultTimeout.store(true, std::memory_order_relaxed);
    }
    baton.turn.store(-1, std::memory_order_release);
    baton.turnDone.store(true, std::memory_order_release);
}

// One peer's lane: spin on its own baton slot and serve every turn until told to stop.
void runAcquireLane(Baton_t& baton, cme::Peer* self, cme::DomainId domain, cme::PeerId pid)
{
    while (true)
    {
        while (baton.turn.load(std::memory_order_acquire) != static_cast<int>(pid))
        {
            if (baton.stopThreads.load(std::memory_order_acquire))
            {
                return;
            }
        }
        serveOneTurn(baton, self, domain);
    }
}

// One thread per peer, parked on its own baton slot until told to acquire @domain.
std::vector<std::thread> spawnAcquireThreads(std::vector<std::unique_ptr<cme::Peer>>& peers,
                                             cme::DomainId domain, Baton_t& baton)
{
    std::vector<std::thread> acq;
    acq.reserve(peers.size());
    for (std::size_t i = 0; i < peers.size(); ++i)
    {
        const auto pid = static_cast<cme::PeerId>(i);
        acq.emplace_back(runAcquireLane, std::ref(baton), peers[i].get(), domain, pid);
    }
    return acq;
}

// Mutable state threaded through the measured loop: current holder + latency samples.
struct DriveState_t
{
    std::int64_t holder{-1};  // single domain -> single holder; -1 = unknown/genesis
    std::vector<std::uint32_t> latAll;
    std::vector<std::uint32_t> latMigrate;
    std::vector<std::uint32_t> latResident;
    std::uint32_t timeouts{0};
};

// Hands the turn to a random (or forced-migrate) peer and waits for it to finish, recording
// the sample when @record. The turnDone-then-baton store order is load-bearing: reversing it
// lets the acquire thread see a stale "done" from the previous turn.
void oneAcquire(const Opts_t& opt, Baton_t& baton, DriveState_t& state, std::uint64_t& rng, bool record)
{
    auto pick = static_cast<cme::PeerId>(harness::nextRandom(rng) % opt.numPeers);
    if (opt.forceMigrate && opt.numPeers > 1 && state.holder >= 0)
    {
        while (static_cast<std::int64_t>(pick) == state.holder)
        {
            pick = static_cast<cme::PeerId>(harness::nextRandom(rng) % opt.numPeers);
        }
    }
    const bool migrate = state.holder >= 0 && static_cast<std::int64_t>(pick) != state.holder;
    baton.turnDone.store(false, std::memory_order_relaxed);
    baton.turn.store(static_cast<int>(pick), std::memory_order_release);  // hand off the turn
    while (!baton.turnDone.load(std::memory_order_acquire))
    {
    }
    state.holder = static_cast<std::int64_t>(pick);
    if (record)
    {
        if (baton.resultTimeout.load(std::memory_order_relaxed))
        {
            ++state.timeouts;
        }
        else
        {
            const std::uint32_t sampleNs = baton.resultNs.load(std::memory_order_relaxed);
            state.latAll.push_back(sampleNs);
            (migrate ? state.latMigrate : state.latResident).push_back(sampleNs);
        }
    }
}

// Unmeasured warmup then the measured loop. No settle sleep: warmup already marks every
// peer Active and lock() blocks anyway; an idle gap would push acquires past a
// --trace-jsonl plot window.
void runLoop(const Opts_t& opt, Baton_t& baton, DriveState_t& state)
{
    std::uint64_t rng = opt.seed * 0x9e3779b97f4a7c15ull + 1;
    state.latAll.reserve(opt.iters);
    for (std::uint32_t i = 0; i < opt.warmup; ++i)
    {
        oneAcquire(opt, baton, state, rng, false);
    }
    for (std::uint32_t i = 0; i < opt.iters; ++i)
    {
        oneAcquire(opt, baton, state, rng, true);
    }
}

// --trace-jsonl: dump the CME_LATENCY span trace (no-op unless built CME_LATENCY).
// Sample the TSC frequency over a short window so the plotter can render sampleNs.
void dumpTraceJsonl(const char* path)
{
    const auto startCycles = cme::time::readTimestampCounter();
    const auto startWall = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const auto endCycles = cme::time::readTimestampCounter();
    const auto endWall = std::chrono::steady_clock::now();
    const double winNs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(endWall - startWall).count());
    const double ghz = winNs > 0 ? static_cast<double>(endCycles - startCycles) / winNs : 0.0;
    cme::trace::writeJsonl(path, ghz);
    std::printf("trace-jsonl -> %s\n", path);
}

void report(const char* tag, std::vector<std::uint32_t>& samples)
{
    std::sort(samples.begin(), samples.end());
    double sum = 0.0;
    for (auto sample : samples)
    {
        sum += sample;
    }
    const double mean = samples.empty() ? 0.0 : sum / static_cast<double>(samples.size());
    std::printf("  %-9s n=%-7zu mean=%8.0f  p50=%8.0f  p90=%8.0f  p99=%8.0f  max=%8.0f  (sampleNs)\n",
                tag, samples.size(), mean, harness::percentile(samples, 0.50), harness::percentile(samples, 0.90),
                harness::percentile(samples, 0.99), harness::percentile(samples, 1.0));
}

void printReport(const Opts_t& opt, std::uint32_t dataDomains, DriveState_t& state,
                 const char* backend)
{
    std::printf("\n=== sequential (uncontended) acquire latency ===\n");
    std::printf("strategy        : %s\n", harness::strategyName(opt.strategy));
    std::printf("backend         : %s\n", backend);
    std::printf("peers           : %u\n", opt.numPeers);
    std::printf("data domains    : %u (slots 1..%u)\n", dataDomains, opt.numDomains - 1);
    std::printf("iters (measured): %u (warmup %u)\n", opt.iters, opt.warmup);
    std::printf("force-migrate   : %s\n", opt.forceMigrate ? "yes" : "no");
    std::printf("timeouts        : %u\n", state.timeouts);
    report("all", state.latAll);
    report("migrate", state.latMigrate);  // sorts latMigrate in place
    report("resident", state.latResident);
}

// --csv: append one sweep row. Plots the MIGRATE cost (pure handoff, the point
// of a sequential bench); falls back to all if no migration happened (n=1).
void writeCsvRow(const Opts_t& opt, std::uint32_t dataDomains, DriveState_t& state)
{
    if (opt.csvPath == nullptr)
    {
        return;
    }
    auto& samples = state.latMigrate.empty() ? state.latAll : state.latMigrate;
    double sum = 0.0;
    for (auto sample : samples)
    {
        sum += sample;
    }
    const double meanUs = samples.empty() ? 0.0 : sum / static_cast<double>(samples.size()) / 1000.0;
    if (FILE* out = std::fopen(opt.csvPath, "a"))
    {
        std::fprintf(out, "%u,%u,%.3f,%.3f,%.3f,%.3f,%zu\n", opt.numPeers, dataDomains,
                     meanUs, harness::percentile(samples, 0.50) / 1000.0, harness::percentile(samples, 0.90) / 1000.0,
                     harness::percentile(samples, 0.99) / 1000.0, samples.size());
        std::fclose(out);
    }
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    Opts_t opt;
    if (parseOpts(ctx.argc(), ctx.argv(), opt) < 0)
    {
        ctx.check(false, "options parsed");
        return;
    }

    const cme::Geometry::FormatOpts_t fmtOpts{opt.strategy};
    cme::Geometry region = ctx.memory().createRegion(opt.maxDomains, opt.maxPeers, fmtOpts);
    harness::seedDataDomains(region, opt.numDomains - 1, ctx.coherency());
    std::vector<std::unique_ptr<cme::Peer>> peers = buildPeers(opt, region, ctx.coherency());

    const cme::DomainId domainId = 1;  // single fixed domain -- only peer order varies
    const std::uint32_t dataDomains = 1;

    Baton_t baton;
    std::vector<std::thread> acq = spawnAcquireThreads(peers, domainId, baton);

    DriveState_t state;
    runLoop(opt, baton, state);

    baton.stopThreads.store(true, std::memory_order_release);
    for (auto& thread : acq)
    {
        thread.join();
    }
    peers.clear();  // stop poll threads before summarising (their span buffers flush on exit)

    if (opt.traceJsonl != nullptr)
    {
        dumpTraceJsonl(opt.traceJsonl);
    }

    printReport(opt, dataDomains, state, ctx.backendName());
    writeCsvRow(opt, dataDomains, state);

    // Bench, not a pass/fail gate: the only thing that fails it is nothing completing.
    ctx.check(!(state.latAll.empty() && state.timeouts > 0), "at least one acquire completed");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
