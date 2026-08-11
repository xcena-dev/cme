// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_fairness.cpp -- N peers x T threads x D domains contended workload.
//
// Each peer runs one Peer instance; its T threads contend on the per-domain local mutex
// (intra-node) plus the CXL ownership (inter-node). Passes if the spread between max and
// min per-peer acquires stays within --bound * mean.
//
//   -n N           peers (default 8, max 64)
//   -d D           domains (default 4, max 64)
//   -t T           threads per peer (default 1)
//   -i ITER        iterations per (peer-thread, domain) (default 500)
//   --warmup W     unmeasured warm-up iters before the gated section (default 0)
//   -c CS          CS busy-spin loop count (default 50)
//   --cs-sleep MS  hold the lock for MS ms (default 0)
//   --asymmetric   peer 0 short CS, others long CS
//   --order|--request|--request-agg   strategy (default request)
//   --bound F      max (max-min)/mean (default 0.15)

// getopt_long is attributed to <bits/getopt_ext.h>, which .clang-tidy ignores, so the
// include-cleaner fixer reads this header as unused and deletes it. The NOLINT stops that.
#include <getopt.h>  // NOLINT(misc-include-cleaner)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "observe/latency.hpp"
#include "test_context.hpp"
#include "test_options.hpp"

namespace test
{
namespace
{

struct Opts_t
{
    std::uint32_t numPeers{8};
    std::uint32_t numDomains{4};
    std::uint32_t maxPeers{0};
    std::uint32_t maxDomains{0};
    std::uint32_t iterPerThread{500};
    std::uint32_t warmupIter{0};  // unmeasured iters before the gated section
    std::uint32_t csLoop{50};
    std::uint32_t csSleepMs{0};
    std::uint32_t threadsPerPeer{1};
    std::uint32_t pollUs{10};  // poll-thread cadence (format header value)
    bool asymmetric{false};
    bool shuffle{false};  // randomise each thread's per-sweep domain order (vs lockstep)
    cme::Strategy strategy{cme::Strategy::Request};
    double bound{0.15};
    const char* latCsv{nullptr};      // dump raw per-acquire latencies (ns) here
    const char* traceJsonl{nullptr};  // CME_LATENCY: dump per-thread event trace here
};

void usage(const char* prog)
{
    std::fprintf(stderr,
                 "usage: %s [-n N] [-d D] [-t T] [-i ITER] [--warmup W] [-c CS]\n"
                 "          [--asymmetric] [--order|--request|--request-agg] [--bound F] [--cs-sleep MS]\n"
                 "          [--max-peers M] [--max-domains M] [--poll-us US]\n",
                 prog);
}

int parseOpts(int argc, char** argv, Opts_t& opt)
{
    enum
    {
        OptOrder = 1000,
        OptRequest,
        OptRequestAgg,
        OptPeterson,
        OptAsym,
        OptShuffle,
        OptBound,
        OptCsSleep,
        OptPollUs,
        OptMaxPeers,
        OptMaxDomains,
        OptLatCsv,
        OptWarmup,
        OptTraceJsonl
    };
    static const struct option LongOpts[] = {
        {"order", no_argument, nullptr, OptOrder},
        {"request", no_argument, nullptr, OptRequest},
        {"request-agg", no_argument, nullptr, OptRequestAgg},
        {"peterson", no_argument, nullptr, OptPeterson},
        {"asymmetric", no_argument, nullptr, OptAsym},
        {"shuffle", no_argument, nullptr, OptShuffle},
        {"bound", required_argument, nullptr, OptBound},
        {"warmup", required_argument, nullptr, OptWarmup},
        {"cs-sleep", required_argument, nullptr, OptCsSleep},
        {"poll-us", required_argument, nullptr, OptPollUs},
        {"max-peers", required_argument, nullptr, OptMaxPeers},
        {"max-domains", required_argument, nullptr, OptMaxDomains},
        {"lat-csv", required_argument, nullptr, OptLatCsv},
        {"trace-jsonl", required_argument, nullptr, OptTraceJsonl},
        {nullptr, 0, nullptr, 0},
    };

    int flag = 0;
    while ((flag = getopt_long(argc, argv, "n:d:t:i:c:h", LongOpts, nullptr)) != -1)
    {
        switch (flag)
        {
            case 'n':
                opt.numPeers = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 'd':
                opt.numDomains = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 't':
                opt.threadsPerPeer = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 'i':
                opt.iterPerThread = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 'c':
                opt.csLoop = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case OptOrder:
                opt.strategy = cme::Strategy::Order;
                break;
            case OptRequest:
                opt.strategy = cme::Strategy::Request;
                break;
            case OptRequestAgg:
                opt.strategy = cme::Strategy::RequestAgg;
                break;
            case OptPeterson:
                opt.strategy = cme::Strategy::Peterson;
                break;
            case OptAsym:
                opt.asymmetric = true;
                break;
            case OptShuffle:
                opt.shuffle = true;
                break;
            case OptBound:
                opt.bound = std::atof(optarg);
                break;
            case OptWarmup:
                opt.warmupIter = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case OptCsSleep:
                opt.csSleepMs = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case OptPollUs:
                opt.pollUs = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case OptMaxPeers:
                opt.maxPeers = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case OptMaxDomains:
                opt.maxDomains = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case OptLatCsv:
                opt.latCsv = optarg;
                break;
            case OptTraceJsonl:
                opt.traceJsonl = optarg;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return -1;
        }
    }
    if (opt.numPeers == 0 || opt.numPeers > cme::MaxPeers || opt.numDomains == 0 ||
        opt.numDomains > cme::MaxDomains)
    {
        std::fprintf(stderr, "out-of-range -n / -d\n");
        return -1;
    }
    // Default provisioned size to the active counts; must be >= active and <= ceiling.
    if (opt.maxPeers == 0)
    {
        opt.maxPeers = opt.numPeers;
    }
    if (opt.maxDomains == 0)
    {
        opt.maxDomains = opt.numDomains;
    }
    if (opt.maxPeers < opt.numPeers || opt.maxPeers > cme::MaxPeers ||
        opt.maxDomains < opt.numDomains || opt.maxDomains > cme::MaxDomains)
    {
        std::fprintf(stderr,
                     "out-of-range --max-peers / --max-domains "
                     "(need active <= max <= ceiling)\n");
        return -1;
    }
    if (opt.threadsPerPeer == 0 || opt.threadsPerPeer > 64)
    {
        std::fprintf(stderr, "out-of-range -t (1..64)\n");
        return -1;
    }
    return 0;
}

void csWork(std::uint32_t iters) noexcept
{
    for (volatile std::uint32_t k = 0; k < iters; ++k)
    {
    }
}

struct PeerResult_t
{
    std::uint64_t totalAcq{0};
    // Acquires in the measured (post-warmup) section only -- the fairness gate.
    // Bench-owned, not telemetry, so warm-up never counts toward the spread.
    std::atomic<std::uint64_t> measuredAcq{0};
    std::uint64_t waitCount{0};
    std::uint64_t waitTime{0};
    std::uint64_t spinTime{0};
    std::uint64_t lockHoldTime{0};
    std::uint64_t wallNs{0};
    // Atomic: every worker thread of this peer bumps it.
    std::atomic<std::uint32_t> deadlineHits{0};
    int returnCode{0};
    // Handoff-latency breakdown (CME_LATENCY): raw TSC cycles + counts,
    // indexed by cme::trace::LatencyStage.
    static constexpr std::size_t LatCount = static_cast<std::size_t>(cme::trace::LatencyStage::Count);
    std::uint64_t latCycles[LatCount]{};
    std::uint64_t latCount[LatCount]{};
    // Token handoff path counters (cumulative, incl warmup) -- which path hands off.
    std::uint64_t xferOnRelease{0}, xferOnPoll{0};
    // Per-acquire wall latency (lock() call -> Guard returned), elapsedNs. Bench-level
    // measurement spanning fast-path AND wait-path, complementing telemetry
    // waitTime (wait-phase only). Filled by workers under latMutex.
    std::vector<std::uint64_t> acqLatNs;
    std::mutex latMutex;
};

struct PeerArgs_t
{
    const Opts_t* opts;
    cme::Geometry* region;
    cme::PeerId peerId;
    cme::CoherencyMode coherency;
    std::atomic<std::uint32_t>* startBarrier;
    std::atomic<std::uint32_t>* warmupBarrier;
    std::atomic<std::uint32_t>* endBarrier;
    PeerResult_t* result;
};

// Count-up gate. Membership must stay static across every phase boundary, so nobody
// leaves until all @participants have arrived.
void awaitBarrier(std::atomic<std::uint32_t>& counter, std::uint32_t participants) noexcept
{
    counter.fetch_add(1, std::memory_order_acq_rel);
    while (counter.load(std::memory_order_acquire) < participants)
    {
        std::this_thread::yield();
    }
}

// Where one thread's measurements land. record is false during warm-up, so a warm-up acquire
// touches neither the latency vector nor the fairness counter.
struct Tally_t
{
    std::vector<std::uint64_t>& localLat;
    PeerResult_t& result;
    bool record;
};

// One acquire plus its critical section.
void lockOnce(cme::Peer& peer, cme::DomainId domainId, const Opts_t& opt, std::uint32_t spinCount,
              const Tally_t& tally)
{
    try
    {
        const timing::Stopwatch waited;
        auto guard = peer.lock(domainId);
        if (tally.record)
        {
            tally.localLat.push_back(static_cast<std::uint64_t>(waited.elapsed().count()));
            tally.result.measuredAcq.fetch_add(1, std::memory_order_relaxed);
        }
        csWork(spinCount);
        if (opt.csSleepMs != 0)
        {
            std::this_thread::sleep_for(timing::Millis{opt.csSleepMs});
        }
    }
    catch (const cme::LockTimeoutError&)
    {
        if (tally.record)
        {
            tally.result.deadlineHits.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Read out after the end barrier, while every peer is still joined.
void collectTelemetry(const cme::Peer& peer, PeerResult_t& result, std::uint64_t wallNs)
{
    const auto stats = peer.getTelemetry();
    result.waitCount = stats.waitCount;
    result.waitTime = stats.waitTime;
    result.spinTime = stats.spinTime;
    result.lockHoldTime = stats.lockHoldTime;
    result.xferOnRelease = stats.transferOnRelease;
    result.xferOnPoll = stats.transferOnPoll;
    for (std::size_t bucket = 0; bucket < PeerResult_t::LatCount; ++bucket)
    {
        result.latCycles[bucket] = stats.handoffLat.cycles[bucket];
        result.latCount[bucket] = stats.handoffLat.count[bucket];
    }
    result.wallNs = wallNs;
    // Gate on measured-section acquires, not cumulative telemetry, so warm-up never
    // counts toward the spread. The telemetry above stays for the diagnostic printout.
    result.totalAcq = result.measuredAcq.load();
}

// Everything one worker thread of a peer reads. Gathered so the thread body can be a named
// function: the alternative nests the body deep enough to hide its own control flow. Every
// member outlives the threads, because the peer that owns them joins before it returns.
struct WorkerContext_t
{
    cme::Peer& peer;
    const Opts_t& opt;
    const PeerArgs_t& arg;
    std::atomic<std::uint64_t>& wallTotal;
    std::uint32_t workerCount;
    std::uint32_t spinCount;
};

// One pass over every domain this peer joined, in this thread's own order. Under --shuffle the
// order is redrawn per sweep, so threads do not traverse domains in lockstep.
void sweepDomains(const WorkerContext_t& shared, std::vector<cme::DomainId>& visit,
                  std::uint64_t& rng, const Tally_t& tally)
{
    if (shared.opt.shuffle)
    {
        harness::shuffleVisitOrder(visit, rng);
    }
    for (const cme::DomainId domainId : visit)
    {
        lockOnce(shared.peer, domainId, shared.opt, shared.spinCount, tally);
    }
}

// One worker thread's whole run: warm up, wait for every worker to finish warming up, then the
// measured sweeps. An exception is recorded rather than left to escape, because a thread that
// leaves its body takes the process down with it.
void runWorker(const WorkerContext_t& shared)
{
    try
    {
        awaitBarrier(*shared.arg.startBarrier, shared.workerCount);

        std::vector<std::uint64_t> localLat;
        localLat.reserve(static_cast<std::size_t>(shared.opt.iterPerThread) *
                         shared.opt.numDomains);

        std::vector<cme::DomainId> visit(shared.opt.numDomains);
        std::iota(visit.begin(), visit.end(), cme::DomainId{0});
        std::uint64_t rng =
            ((static_cast<std::uint64_t>(shared.arg.peerId) + 1) * 0x9e3779b97f4a7c15ull) ^
            std::hash<std::thread::id>{}(std::this_thread::get_id());

        for (std::uint32_t sweep = 0; sweep < shared.opt.warmupIter; ++sweep)
        {
            sweepDomains(shared, visit, rng, Tally_t{localLat, *shared.arg.result, false});
        }
        // Enter the measured section only once every worker has finished warm-up, so startup
        // skew stays out of the gated spread.
        awaitBarrier(*shared.arg.warmupBarrier, shared.workerCount);

        cme::trace::setMeasuring(true);  // spans past here tagged measured=true
        const timing::Stopwatch measured;
        for (std::uint32_t sweep = 0; sweep < shared.opt.iterPerThread; ++sweep)
        {
            sweepDomains(shared, visit, rng, Tally_t{localLat, *shared.arg.result, true});
        }
        shared.wallTotal.fetch_add(static_cast<std::uint64_t>(measured.elapsed().count()));

        const std::lock_guard<std::mutex> merge(shared.arg.result->latMutex);
        auto& dst = shared.arg.result->acqLatNs;
        dst.insert(dst.end(), localLat.begin(), localLat.end());
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "peer %u worker exception: %s\n", shared.arg.peerId, e.what());
        shared.arg.result->returnCode = 1;
    }
}

void runPeer(PeerArgs_t arg)
{
    auto& opt = *arg.opts;
    try
    {
        cme::Peer peer{*arg.region, arg.peerId, arg.coherency};
        // Opt-in: join the data domains (slots 1..numDomains-1) this peer locks;
        // slot 0 (control) is joined by default.
        for (cme::DomainId domainId = 1; domainId < opt.numDomains; ++domainId)
        {
            peer.joinDomain(domainId);
        }

        const std::uint32_t spinCount =
            opt.asymmetric ? (arg.peerId == 0 ? opt.csLoop / 10 + 1 : opt.csLoop * 5) : opt.csLoop;

        std::atomic<std::uint64_t> wallTotal{0};
        const std::uint32_t workerCount = opt.numPeers * opt.threadsPerPeer;

        const WorkerContext_t shared{peer, opt, arg, wallTotal, workerCount, spinCount};
        // This thread is one of the workers, so it runs a body itself between the spawn and the
        // wait. The explicit join is what the barrier below needs: it must not open until every
        // helper of this peer has stopped acquiring.
        harness::ThreadGroup helpers;
        helpers.spawn(opt.threadsPerPeer - 1,
                      [&shared](std::uint32_t)
                      {
                          runWorker(shared);
                      });
        runWorker(shared);
        helpers.join();

        // An early leaver flips its slot to None, and a still-measuring peer's successor
        // cache could then hand the token to it. Keep membership static.
        awaitBarrier(*arg.endBarrier, opt.numPeers);

        collectTelemetry(peer, *arg.result, wallTotal.load());
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "peer %u: %s\n", arg.peerId, e.what());
        arg.result->returnCode = 1;
    }
}

void runPeers(const Opts_t& opt, cme::Geometry& region, std::vector<PeerResult_t>& results,
              cme::CoherencyMode coherency)
{
    std::atomic<std::uint32_t> startBarrier{0};
    std::atomic<std::uint32_t> warmupBarrier{0};
    std::atomic<std::uint32_t> endBarrier{0};

    std::vector<std::thread> peers;
    peers.reserve(opt.numPeers);
    for (cme::PeerId pid = 0; pid < opt.numPeers; ++pid)
    {
        const PeerArgs_t args{&opt, &region, pid, coherency,
                              &startBarrier, &warmupBarrier, &endBarrier, &results[pid]};
        peers.emplace_back(runPeer, args);
    }
    for (auto& thread : peers)
    {
        thread.join();
    }
}

// The fairness gate: acquire spread across peers, plus the deadline-hit total.
struct Spread_t
{
    std::uint64_t maxAcq{0};
    std::uint64_t minAcq{UINT64_MAX};
    double mean{0.0};
    double dev{0.0};
    std::uint32_t deadlineHits{0};
};

Spread_t computeSpread(const std::vector<PeerResult_t>& results)
{
    Spread_t spread;
    std::uint64_t sumAcq = 0;
    for (const auto& result : results)
    {
        spread.maxAcq = std::max(spread.maxAcq, result.totalAcq);
        spread.minAcq = std::min(spread.minAcq, result.totalAcq);
        sumAcq += result.totalAcq;
        spread.deadlineHits += result.deadlineHits.load();
    }
    spread.mean = static_cast<double>(sumAcq) / static_cast<double>(results.size());
    spread.dev = spread.mean > 0 ? static_cast<double>(spread.maxAcq - spread.minAcq) / spread.mean : 0.0;
    return spread;
}

void printConfig(const Opts_t& opt)
{
    std::printf("\n=== fairness summary ===\n");
    std::printf("strategy        : %s\n", harness::strategyName(opt.strategy));
    std::printf("peers           : %u (provisioned max_peers %u)\n", opt.numPeers, opt.maxPeers);
    std::printf("domains         : %u (provisioned max_domains %u)\n", opt.numDomains,
                opt.maxDomains);
    std::printf("threads/peer    : %u\n", opt.threadsPerPeer);
    std::printf("iter/(p-thread,d): %u (warmup %u)\n", opt.iterPerThread, opt.warmupIter);
    std::printf("cs_loop         : %u (%s)\n", opt.csLoop,
                opt.asymmetric ? "asymmetric: peer 0 short, rest long" : "symmetric");
    std::printf("cs_sleep_ms     : %u\n", opt.csSleepMs);
    std::printf("expected/peer   : %u\n",
                opt.iterPerThread * opt.numDomains * opt.threadsPerPeer);
}

void printPerPeerTotals(const std::vector<PeerResult_t>& results)
{
    std::printf("\nper-peer totals:\n");
    for (std::uint32_t peerIndex = 0; peerIndex < results.size(); ++peerIndex)
    {
        const auto& result = results[peerIndex];
        const auto avgWaitUs =
            result.waitCount ? static_cast<double>(result.waitTime) / static_cast<double>(result.waitCount) / 1000.0 : 0.0;
        const auto avgHoldUs =
            result.totalAcq ? static_cast<double>(result.lockHoldTime) / static_cast<double>(result.totalAcq) / 1000.0 : 0.0;
        const auto spinPct =
            result.waitTime ? static_cast<double>(result.spinTime) / static_cast<double>(result.waitTime) * 100.0 : 0.0;
        std::printf("  peer %u: acq=%" PRIu64 " wait_n=%" PRIu64
                    " avg_wait=%.1fus avg_hold=%.1fus wall=%.1fms spin=%.1f%% dl=%u rc=%d\n",
                    peerIndex, result.totalAcq, result.waitCount, avgWaitUs, avgHoldUs,
                    static_cast<double>(result.wallNs) / 1e6, spinPct, result.deadlineHits.load(),
                    result.returnCode);
    }
}

void dumpLatencyCsv(const char* path, const std::vector<std::uint64_t>& sortedLat)
{
    std::FILE* out = std::fopen(path, "w");
    if (out == nullptr)
    {
        std::fprintf(stderr, "lat-csv: cannot open %s\n", path);
        return;
    }
    std::fprintf(out, "latency_ns\n");
    for (const auto sample : sortedLat)
    {
        std::fprintf(out, "%" PRIu64 "\n", sample);
    }
    std::fclose(out);
    std::printf("lat-csv -> %s (%zu samples)\n", path, sortedLat.size());
}

// Perf signal, not a gate.
void printLatencyDistribution(const Opts_t& opt, const std::vector<PeerResult_t>& results)
{
    std::vector<std::uint64_t> allLat;
    for (const auto& result : results)
    {
        allLat.insert(allLat.end(), result.acqLatNs.begin(), result.acqLatNs.end());
    }
    if (allLat.empty())
    {
        return;
    }
    std::sort(allLat.begin(), allLat.end());
    const std::size_t count = allLat.size();
    const auto pct = [&](double fraction) -> double
    {
        const auto index = static_cast<std::size_t>(fraction * static_cast<double>(count - 1));
        return static_cast<double>(allLat[index]) / 1000.0;
    };
    std::uint64_t sum = 0;
    for (const auto sample : allLat)
    {
        sum += sample;
    }
    std::printf(
        "\nacquire latency (us, n=%zu): mean=%.3f p50=%.3f p90=%.3f "
        "p99=%.3f p999=%.3f max=%.3f\n",
        count, static_cast<double>(sum) / static_cast<double>(count) / 1000.0, pct(0.50), pct(0.90), pct(0.99),
        pct(0.999), static_cast<double>(allLat.back()) / 1000.0);

    if (opt.latCsv != nullptr)
    {
        dumpLatencyCsv(opt.latCsv, allLat);
    }
}

#if defined(CME_LATENCY)
void printHandoffBreakdown(const std::vector<PeerResult_t>& results)
{
    const double ghz = measureTscGhz();

    constexpr std::size_t latCount = PeerResult_t::LatCount;
    std::uint64_t cyc[latCount] = {};
    std::uint64_t cnt[latCount] = {};
    std::uint64_t xferRel = 0;
    std::uint64_t xferPoll = 0;
    for (const auto& result : results)
    {
        xferRel += result.xferOnRelease;
        xferPoll += result.xferOnPoll;
        for (std::size_t i = 0; i < latCount; ++i)
        {
            cyc[i] += result.latCycles[i];
            cnt[i] += result.latCount[i];
        }
    }
    const auto avgNs = [&](std::uint64_t c, std::uint64_t count) -> double
    {
        return (count == 0 || ghz == 0.0) ? 0.0 : static_cast<double>(c) / count / ghz;
    };

    const std::uint64_t xferTotal = xferRel + xferPoll;
    const double relPct = xferTotal ? 100.0 * static_cast<double>(xferRel) / xferTotal : 0.0;
    std::printf("\nhandoff latency breakdown (avg elapsedNs, tsc=%.3f GHz):\n", ghz);
    std::printf("token handoff path: on_release=%" PRIu64 " (%.1f%%)  on_poll=%" PRIu64
                " (%.1f%%)\n",
                xferRel, relPct, xferPoll, 100.0 - relPct);
    // One line per LatencyStage; n=0 means unmeasured in this build.
    for (std::size_t i = 0; i < latCount; ++i)
    {
        const auto stage = static_cast<cme::trace::LatencyStage>(i);
        std::printf("  %-14s: %8.1f  (n=%" PRIu64 ")\n", cme::trace::getLatencyName(stage),
                    avgNs(cyc[i], cnt[i]), cnt[i]);
    }
}
#endif

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

    // Slot 0 is control; create the rest so workers find every slot Active. Control doubles
    // as a contended domain here, since no create/delete runs during the measured section.
    harness::seedDataDomains(region, opt.numDomains - 1);

    std::vector<PeerResult_t> results(opt.numPeers);
    runPeers(opt, region, results, ctx.coherency());

    // Every peer thread has exited, so the worker and poll trace buffers are flushed.
    if (opt.traceJsonl != nullptr)
    {
        harness::dumpLatencyTrace(opt.traceJsonl);
    }

    const Spread_t spread = computeSpread(results);
    printConfig(opt);
    printPerPeerTotals(results);
    printLatencyDistribution(opt, results);
#if defined(CME_LATENCY)
    printHandoffBreakdown(results);
#endif

    std::printf("\nbound check:\n");
    std::printf("  max=%" PRIu64 "  min=%" PRIu64 "  mean=%.1f  (max-min)/mean=%.3f  bound=%.3f\n",
                spread.maxAcq, spread.minAcq, spread.mean, spread.dev, opt.bound);

    if (spread.deadlineHits != 0)
    {
        std::printf("  deadline hits: %u\n", spread.deadlineHits);
    }
    ctx.check(spread.dev <= opt.bound, "spread stayed inside the fairness bound");
    ctx.check(spread.deadlineHits == 0, "no acquire hit its deadline");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
