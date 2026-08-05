// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_tiered_lock.cpp -- the two-tier lock contract via SharedSession.
//
// cme::Peer alone excludes across nodes but not across threads of the same peer -- they all
// see their peer as resident holder and enter together. cme::SharedSession adds the missing
// per-domain intra-node tier. This test drives both tiers: N sessions x T threads each.
//
// Each thread does a non-atomic RMW on one shared counter inside the CS, so both tiers
// holding means exactly N*T*M; a lost update shows up short. Also asserts bounded-wait.
//
// Strategy from --strategy (helper strategyChoice).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "args.hpp"
#include "cme/shared.hpp"
#include "cme/shared_session.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

// Sizes default to a modest ctest load; override on the command line to sweep
// performance: --peers, --threads, --iters, --domains, --cohort-cap, --shuffle.
std::uint32_t readOptU32(const char* flag, std::uint32_t fallback)
{
    return static_cast<std::uint32_t>(harness::argU64(flag, fallback));
}

// Per data domain [1..D] counter; non-atomic on purpose (the two-tier lock must
// serialize each domain's RMW). Sized in main. Threads holding DIFFERENT domains touch
// different elements, so a lost update only appears if a domain has two holders at once.
std::vector<std::uint64_t> g_counter;

// The critical section itself, plus the acquire-latency sample. Called with both tiers held,
// so @began is the instant the acquire started.
void recordCriticalSection(std::uint32_t domainId, std::chrono::steady_clock::time_point began,
                           std::vector<std::uint32_t>& localLat)
{
    const auto held = std::chrono::steady_clock::now();
    ++g_counter[domainId];  // non-atomic on purpose: a lost update means ME broke
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(held - began).count();
    localLat.push_back(static_cast<std::uint32_t>(std::min<std::int64_t>(elapsedNs, UINT32_MAX)));
}

struct Config_t
{
    std::uint32_t numPeers{8};
    std::uint32_t threadsPerPeer{4};
    std::uint32_t itersPerThread{1000};
    std::uint32_t numDomains{1};
    std::uint32_t domainCeiling{2};
    std::uint32_t cohortCap{4};
    bool shuffle{false};
};

Config_t readConfig()
{
    Config_t cfg;
    cfg.numPeers = static_cast<std::uint32_t>(readOptU32("--peers", 8));
    cfg.threadsPerPeer = readOptU32("--threads", 4);
    cfg.itersPerThread = readOptU32("--iters", 1000);
    // An over-large D is not clamped here: Session::format rejects it with FormatError, which
    // is the library's own ceiling rather than a copy of it.
    cfg.numDomains = readOptU32("--domains", 1);
    cfg.domainCeiling = cfg.numDomains + 1;  // control(0) + D data domains
    cfg.cohortCap = readOptU32("--cohort-cap", 4);
    cfg.shuffle = readOptU32("--shuffle", 0) != 0;
    return cfg;
}

// seedDataDomains names data domain ids 1..D "lane0".."lane{D-1}"; SharedSession locks by name.
std::string domainName(std::uint32_t domainId)
{
    return "lane" + std::to_string(domainId - 1);
}

struct Tiers_t
{
    std::vector<cme::SharedSession> shared;  // one per simulated node
};

// Built and joined up front so every peer is Active before the threads race. One SharedSession
// per node, its T threads sharing it: the per-domain mutex inside is the intra-node tier, and
// cohorting bounds how long remote peers wait for the handoff.
Tiers_t buildTiers(const Config_t& cfg, const std::string& uri)
{
    Tiers_t tiers;
    tiers.shared.reserve(cfg.numPeers);
    for (std::uint32_t pid = 0; pid < cfg.numPeers; ++pid)
    {
        tiers.shared.emplace_back(cme::SharedSession::open(uri));
        tiers.shared[pid].setCohortCap(cfg.cohortCap);
        for (std::uint32_t domainId = 1; domainId <= cfg.numDomains; ++domainId)
        {
            if (pid == 0)
            {
                tiers.shared[0].createDomain(domainName(domainId));  // create does not join
            }
            tiers.shared[pid].joinDomain(domainName(domainId));
        }
    }
    return tiers;
}

// One thread's place in the run: whose SharedSession it shares, and which counter slot it owns.
struct Worker_t
{
    std::uint32_t pid{0};
    std::size_t slot{0};
};

// One thread's whole run: itersPerThread sweeps, each locking every data domain once.
void runSweeps(const Config_t& cfg, Tiers_t& tiers, Worker_t worker,
               std::vector<std::uint32_t>& done, std::vector<std::uint32_t>& localLat)
{
    std::vector<std::uint32_t> visit(cfg.numDomains);
    for (std::uint32_t k = 0; k < cfg.numDomains; ++k)
    {
        visit[k] = k + 1;
    }
    std::uint64_t rng = (static_cast<std::uint64_t>(worker.slot) + 1) * 0x9e3779b97f4a7c15ull;

    for (std::uint32_t i = 0; i < cfg.itersPerThread; ++i)
    {
        if (cfg.shuffle)
        {
            harness::shuffleVisitOrder(visit, rng);
        }
        for (const std::uint32_t domainId : visit)
        {
            const auto began = std::chrono::steady_clock::now();
            {
                // local mutex -> CXL
                auto guard = tiers.shared[worker.pid].lock(domainName(domainId));
                recordCriticalSection(domainId, began, localLat);
            }
        }
        ++done[worker.slot];
    }
}

void runThreads(const Config_t& cfg, Tiers_t& tiers, std::vector<std::uint32_t>& done,
                std::vector<std::uint32_t>& acqLatNs, std::atomic<int>& failures)
{
    std::mutex latMutex;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(cfg.numPeers) * cfg.threadsPerPeer);

    for (std::uint32_t pid = 0; pid < cfg.numPeers; ++pid)
    {
        for (std::uint32_t index = 0; index < cfg.threadsPerPeer; ++index)
        {
            const std::size_t slot = static_cast<std::size_t>(pid) * cfg.threadsPerPeer + index;
            threads.emplace_back(
                [&, pid, slot]()
                {
                    try
                    {
                        std::vector<std::uint32_t> localLat;
                        localLat.reserve(static_cast<std::size_t>(cfg.itersPerThread) *
                                         cfg.numDomains);
                        runSweeps(cfg, tiers, Worker_t{pid, slot}, done, localLat);
                        const std::lock_guard<std::mutex> merge(latMutex);
                        acqLatNs.insert(acqLatNs.end(), localLat.begin(), localLat.end());
                    }
                    catch (const std::exception& e)
                    {
                        std::fprintf(stderr, "peer %u thread %zu: %s\n", pid, slot, e.what());
                        failures.fetch_add(1);
                    }
                });
        }
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
}

// Each domain is locked once per (thread, iter), so a correct two-tier lock leaves every
// domain at exactly N*T*M. Any lost update means it admitted two holders.
[[nodiscard]] bool countersMatch(const Config_t& cfg, const char* stratSuffix)
{
    const std::uint64_t perDomain =
        static_cast<std::uint64_t>(cfg.numPeers) * cfg.threadsPerPeer * cfg.itersPerThread;
    std::uint64_t total = 0;
    bool meOk = true;
    for (std::uint32_t domainId = 1; domainId <= cfg.numDomains; ++domainId)
    {
        total += g_counter[domainId];
        meOk = meOk && g_counter[domainId] == perDomain;
    }
    const std::uint64_t expected = perDomain * cfg.numDomains;
    std::printf(
        "strategy=%s peers=%u threads/peer=%u domains=%u iters=%u "
        "counter=%" PRIu64 " expected=%" PRIu64 "\n",
        stratSuffix, cfg.numPeers, cfg.threadsPerPeer, cfg.numDomains, cfg.itersPerThread, total,
        expected);
    return meOk && total == expected;
}

// Two-tier acquire latency: intra-node mutex wait plus inter-node CXL handoff.
void reportLatency(const Config_t& cfg, std::vector<std::uint32_t>& acqLatNs)
{
    std::sort(acqLatNs.begin(), acqLatNs.end());
    double sum = 0.0;
    for (const auto sample : acqLatNs)
    {
        sum += sample;
    }
    const double meanNs = acqLatNs.empty() ? 0.0 : sum / static_cast<double>(acqLatNs.size());
    std::printf("acquire latency : n=%zu mean=%.0f p50=%.0f p90=%.0f p99=%.0f max=%.0f (ns)\n",
                acqLatNs.size(), meanNs, harness::percentile(acqLatNs, 0.50), harness::percentile(acqLatNs, 0.90),
                harness::percentile(acqLatNs, 0.99), harness::percentile(acqLatNs, 1.0));

    // --csv <path>: append one sweep row (us). Columns: peers,threads,domains + latency.
    const std::string csv = harness::argStr("--csv", std::string{});
    if (csv.empty())
    {
        return;
    }
    if (std::FILE* out = std::fopen(csv.c_str(), "a"))
    {
        std::fprintf(out, "%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%zu\n", cfg.numPeers, cfg.threadsPerPeer,
                     cfg.numDomains, meanNs / 1000.0, harness::percentile(acqLatNs, 0.50) / 1000.0,
                     harness::percentile(acqLatNs, 0.90) / 1000.0, harness::percentile(acqLatNs, 0.99) / 1000.0,
                     acqLatNs.size());
        std::fclose(out);
    }
}

// Sessions first -- their poll threads touch the region -- then the caller removes the backing
// object. dax is a fixed device, so leave it alone.
void teardown(Tiers_t& tiers)
{
    tiers.shared.clear();
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const cme::Strategy strategy = ctx.strategy();
    const char* const stratSuffix = ctx.strategySuffix();

    const Config_t cfg = readConfig();
    g_counter.assign(cfg.numDomains + 1, 0);  // index 1..D used

    const std::string& uri = ctx.uri();
    cme::Session::FormatOpts_t fmtOpts{};
    fmtOpts.maxDomains = cfg.domainCeiling;  // control(0) + D data domains
    fmtOpts.maxPeers = cfg.numPeers;
    fmtOpts.strategy = strategy;
    cme::Session::format(uri, fmtOpts);

    Tiers_t tiers = buildTiers(cfg, uri);

    // Let membership settle before the threads race. Without this, early acquires can
    // misroute to a not-yet-Active peer and strand the token into starvation timeouts.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    std::vector<std::uint32_t> done(static_cast<std::size_t>(cfg.numPeers) * cfg.threadsPerPeer, 0);
    std::vector<std::uint32_t> acqLatNs;  // merged per-acquire lock-acquire latencies (ns)
    std::atomic<int> failures{0};
    runThreads(cfg, tiers, done, acqLatNs, failures);

    const bool meOk = countersMatch(cfg, stratSuffix);
    ctx.check(failures.load() == 0, "every thread ran without exception");
    ctx.check(meOk, "two-tier ME: no lost update (each domain == N*T*M)");
    ctx.check(std::all_of(done.begin(), done.end(),
                          [&](std::uint32_t sweeps)
                          {
                              return sweeps == cfg.itersPerThread;
                          }),
              "bounded-wait: every thread completed all M iters (no starvation)");

    reportLatency(cfg, acqLatNs);
    teardown(tiers);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
