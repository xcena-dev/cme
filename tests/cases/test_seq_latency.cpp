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
#include <vector>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "test_options.hpp"

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

// Hands exactly one lock request at a time to a dedicated acquire thread per peer, so the
// trace splits into per-peer lanes instead of collapsing to one. Store order and memory
// order below are load-bearing -- see runAcquireLane/oneAcquire.
struct Baton_t
{
    std::atomic<int> turn{-1};  // peer id whose turn it is (-1 = idle)
    std::atomic<bool> stopThreads{false};
    std::atomic<bool> turnDone{false};
    std::atomic<std::uint64_t> resultNs{0};
    std::atomic<bool> resultTimeout{false};
};

// This peer's turn: time the acquire, publish the sample, then hand the baton back. The
// Guard's inner scope and the -1-then-turnDone store order are load-bearing: reversing either
// lets the coordinator start the next turn before this peer's release lands.
void serveOneTurn(Baton_t& baton, cme::Peer* self, cme::DomainId domain)
{
    try
    {
        const timing::Stopwatch waited;
        {
            auto guard = self->lock(domain);
            baton.resultNs.store(static_cast<std::uint64_t>(waited.elapsed().count()));
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

// Mutable state threaded through the measured loop: current holder + latency samples.
struct DriveState_t
{
    std::int64_t holder{-1};  // single domain -> single holder; -1 = unknown/genesis
    std::vector<std::uint64_t> latAll;
    std::vector<std::uint64_t> latMigrate;
    std::vector<std::uint64_t> latResident;
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

void report(const char* tag, std::vector<std::uint64_t>& samples)
{
    std::sort(samples.begin(), samples.end());
    double sum = 0.0;
    for (auto sample : samples)
    {
        sum += static_cast<double>(sample);
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
        sum += static_cast<double>(sample);
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
    harness::seedDataDomains(region, opt.numDomains - 1);
    // Every peer is a live instance with its own poll thread, which is what forwards and grants a
    // handoff. The driver below is the sole locker, so nothing here contends.
    // numDomains counts the control slot, so the data domains they join are 1..numDomains-1.
    auto peers = harness::makePeers(region, opt.numPeers, opt.numDomains - 1);

    const cme::DomainId domainId = 1;  // single fixed domain -- only peer order varies
    const std::uint32_t dataDomains = 1;

    Baton_t baton;
    // One thread per peer, each parked on its own baton slot. Joined after the drive loop below,
    // since that loop is what hands them the turns they are waiting for.
    harness::ThreadGroup acq;
    acq.spawn(static_cast<std::uint32_t>(peers.size()),
              [&baton, &peers, domainId](std::uint32_t pid)
              {
                  runAcquireLane(baton, peers[pid].get(), domainId, pid);
              });

    DriveState_t state;
    runLoop(opt, baton, state);

    baton.stopThreads.store(true, std::memory_order_release);
    acq.join();
    peers.clear();  // stop poll threads before summarising (their span buffers flush on exit)

    if (opt.traceJsonl != nullptr)
    {
        harness::dumpLatencyTrace(opt.traceJsonl);
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
