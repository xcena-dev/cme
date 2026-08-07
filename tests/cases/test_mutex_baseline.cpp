// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_mutex_baseline.cpp -- process-shared pthread_mutex acquire-latency floor.
//
// The coherent-DRAM / OS-mutex reference point for CME's own shm-backend acquire
// numbers, mirroring rdma_lock.sh's RDMA-CAS reference for the inter-node
// case: what the same access pattern (N peers x D page-spaced locks, fresh
// shuffled sweep per round, trivial CS) costs when the medium is plain coherent
// DRAM and the primitive is PTHREAD_PROCESS_SHARED + PTHREAD_MUTEX_ROBUST instead
// of CME's SWOT protocol. Single host, no fabric -- a floor, not a competitor.
//
// Peers are real fork()ed processes (not threads) so the mutex is genuinely
// exercised across an address-space boundary, matching CME's peer=process model.
//
//   -n N        contending processes (peers), default 8
//   -d D        page-spaced mutexes (domains), default 4
//   -i ITER     measured sweeps per peer (samples = iters*D), default 1000
//   --warmup W  unmeasured sweeps before the measured section, default 100
//   --csv PATH  append one sweep row: peers,domains,mean_us,p50_us,p90_us,p99_us,samples
//   --verify    correctness soak instead of the latency sweep: non-atomic counter
//               RMW under the lock, checked against peers*iters per domain

// Blank line below on purpose: it stops clang-format sorting the includes into the guard,
// where a build that already defines _GNU_SOURCE would skip every one of them.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// getopt_long is attributed to <bits/getopt_ext.h>, which .clang-tidy ignores, so the
// include-cleaner fixer reads this header as unused and deletes it. The NOLINT stops that.
#include <getopt.h>  // NOLINT(misc-include-cleaner)
#include <pthread.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// This baseline opens no cme region, but it still runs through the case harness: that is
// what takes the registration flags out of argv before getopt_long sees them.
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr std::size_t PageSize = 4096;
constexpr std::uint32_t CsLoop = 50;  // trivial CS, matches CME fairness bench default

struct Opts_t
{
    std::uint32_t peers{8};
    std::uint32_t domains{4};
    std::uint32_t iters{1000};
    std::uint32_t warmup{100};
    const char* csvPath{nullptr};
    bool verify{false};
};

int parseOpts(int argc, char** argv, Opts_t& opts)
{
    static const option Long[] = {
        {"warmup", required_argument, nullptr, 'w'},
        {"csv", required_argument, nullptr, 'C'},
        {"verify", no_argument, nullptr, 'V'},
        {nullptr, 0, nullptr, 0},
    };
    int parsed = 0;
    while ((parsed = getopt_long(argc, argv, "n:d:i:w:C:h", Long, nullptr)) != -1)
    {
        switch (parsed)
        {
            case 'n':
                opts.peers = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 'd':
                opts.domains = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 'i':
                opts.iters = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 'w':
                opts.warmup = static_cast<std::uint32_t>(std::atoi(optarg));
                break;
            case 'C':
                opts.csvPath = optarg;
                break;
            case 'V':
                opts.verify = true;
                break;
            case 'h':
            default:
                std::fprintf(stderr,
                             "usage: %s [-n peers] [-d domains] [-i iters] [--warmup W] "
                             "[--csv PATH] [--verify]\n",
                             argv[0]);
                return -1;
        }
    }
    if (opts.peers == 0 || opts.domains == 0)
    {
        std::fprintf(stderr, "need -n >= 1 and -d >= 1\n");
        return -1;
    }
    return 0;
}

// One gate per section, counted up by every peer. Plain uint32_t rather than std::atomic:
// the harness hands back a zeroed mapping, and no process runs a constructor over what a
// sibling already mapped.
struct Control_t
{
    std::uint32_t startBarrier;
    std::uint32_t warmupBarrier;
};

// Count-up gate mirroring test_fairness.cpp's awaitBarrier, just cross-process.
void awaitBarrier(std::uint32_t& counter, std::uint32_t participants) noexcept
{
    __atomic_fetch_add(&counter, 1u, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&counter, __ATOMIC_ACQUIRE) < participants)
    {
        sched_yield();
    }
}

void csWork() noexcept
{
    for (volatile std::uint32_t spin = 0; spin < CsLoop; ++spin)
    {
    }
}

// One mutex per page, matching the RDMA/CME domain-independence convention: two domains
// must never share a line, so nothing measured here is false sharing. mmap hands back a
// page-aligned base, so this alignment alone puts domain @domainIndex that many pages in.
struct alignas(PageSize) PagedMutex_t
{
    pthread_mutex_t mutex;
};

// The shared state a run needs, one named shm segment per piece. The harness owns each
// segment and unlinks it from the process that made it, so a forked peer unmapping on exit
// takes nothing away from its siblings.
struct Region_t
{
    harness::SharedBuffer<Control_t> controlBlock;  // [1]
    harness::SharedBuffer<PagedMutex_t> mutexes;    // [domains]
    harness::SharedBuffer<std::uint64_t> counters;  // [domains], --verify only
    harness::SharedBuffer<std::uint64_t> results;   // [peers * iters * domains] ns, sweep only

    [[nodiscard]] Control_t& control() const noexcept
    {
        return controlBlock[0];
    }

    [[nodiscard]] pthread_mutex_t* mutexAt(std::uint32_t domainIndex) const noexcept
    {
        return &mutexes[domainIndex].mutex;
    }

    // Falsy when any segment this run needs failed to map. Verify and sweep each allocate
    // only their own array, so the other one is legitimately absent.
    [[nodiscard]] bool mapped(const Opts_t& opt) const noexcept
    {
        const bool payload = opt.verify ? static_cast<bool>(counters) : static_cast<bool>(results);
        return static_cast<bool>(controlBlock) && static_cast<bool>(mutexes) && payload;
    }
};

// Process-shared + robust, so a peer that dies holding a mutex leaves it recoverable. No
// peer crashes in this bench, but the primitive should match what a real deployment needs.
bool initMutexes(const Region_t& region, std::uint32_t domains)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    bool valid = true;
    for (std::uint32_t domainIndex = 0; domainIndex < domains && valid; ++domainIndex)
    {
        valid = pthread_mutex_init(region.mutexAt(domainIndex), &attr) == 0;
    }
    pthread_mutexattr_destroy(&attr);
    return valid;
}

Region_t openRegion(const harness::TestContext& ctx, const Opts_t& opt)
{
    Region_t region;
    region.controlBlock = ctx.scratch<Control_t>("control", 1);
    region.mutexes = ctx.scratch<PagedMutex_t>("mutexes", opt.domains);
    if (opt.verify)
    {
        region.counters = ctx.scratch<std::uint64_t>("counters", opt.domains);
    }
    else
    {
        region.results = ctx.scratch<std::uint64_t>(
            "results", static_cast<std::uint64_t>(opt.peers) * opt.iters * opt.domains);
    }
    return region;
}

// Take @mutex, tolerating a robust-mutex EOWNERDEAD handoff (unused in this bench -- no peer
// crashes -- but the primitive is robust, so handle it right).
void lockOrDie(pthread_mutex_t* mutex, std::uint32_t peerIndex)
{
    const int exitCode = pthread_mutex_lock(mutex);
    if (exitCode == EOWNERDEAD)
    {
        pthread_mutex_consistent(mutex);
        return;
    }
    if (exitCode != 0)
    {
        std::fprintf(stderr, "peer %u: mutex_lock: %s\n", peerIndex, std::strerror(exitCode));
        _exit(1);
    }
}

// One forked peer: warmup sweeps (unmeasured), a start + warmup barrier so
// fork/startup skew stays out of the gated section, then the measured sweeps.
void runPeer(const Opts_t& opt, Region_t& region, std::uint32_t peerIndex)
{
    std::vector<std::uint32_t> visitOrder(opt.domains);
    for (std::uint32_t i = 0; i < opt.domains; ++i)
    {
        visitOrder[i] = i;
    }
    std::uint64_t rng = (static_cast<std::uint64_t>(peerIndex) + 1) * 0x9e3779b97f4a7c15ull;

    awaitBarrier(region.control().startBarrier, opt.peers);

    for (std::uint32_t sweep = 0; sweep < opt.warmup; ++sweep)
    {
        harness::shuffleVisitOrder(visitOrder, rng);
        for (const std::uint32_t domainIndex : visitOrder)
        {
            lockOrDie(region.mutexAt(domainIndex), peerIndex);
            csWork();
            pthread_mutex_unlock(region.mutexAt(domainIndex));
        }
    }

    awaitBarrier(region.control().warmupBarrier, opt.peers);

    std::size_t sampleIndex = 0;
    const std::size_t sampleBase = static_cast<std::size_t>(peerIndex) * opt.iters * opt.domains;
    for (std::uint32_t sweep = 0; sweep < opt.iters; ++sweep)
    {
        harness::shuffleVisitOrder(visitOrder, rng);
        for (const std::uint32_t domainIndex : visitOrder)
        {
            const auto acquireStart = std::chrono::steady_clock::now();
            lockOrDie(region.mutexAt(domainIndex), peerIndex);
            const auto acquireEnd = std::chrono::steady_clock::now();
            csWork();
            if (opt.verify)
            {
                region.counters[domainIndex] += 1;  // non-atomic: exclusion must prevent loss
            }
            pthread_mutex_unlock(region.mutexAt(domainIndex));
            if (!opt.verify)
            {
                region.results[sampleBase + sampleIndex++] =
                    harness::nsBetween(acquireStart, acquireEnd);
            }
        }
    }
    _exit(0);
}

// One run's acquire-latency distribution, in the order the CSV header names it.
struct LatencySummary_t
{
    double meanUs;
    double p50;
    double p90;
    double p99;
    std::size_t samples;
};

void writeCsvRow(const Opts_t& opt, const LatencySummary_t& latency)
{
    if (opt.csvPath == nullptr)
    {
        return;
    }
    if (FILE* csv = std::fopen(opt.csvPath, "a"))
    {
        std::fprintf(csv, "%u,%u,%.3f,%.3f,%.3f,%.3f,%zu\n", opt.peers, opt.domains,
                     latency.meanUs, latency.p50, latency.p90, latency.p99, latency.samples);
        std::fclose(csv);
    }
}

// Fork one process per peer, then wait for all of them. Returns how many exited non-zero.
int runPeers(const Opts_t& opt, Region_t& region)
{
    std::vector<pid_t> children;
    children.reserve(opt.peers);
    for (std::uint32_t peerIndex = 0; peerIndex < opt.peers; ++peerIndex)
    {
        const pid_t pid = fork();
        if (pid < 0)
        {
            std::perror("fork");
            return 1;
        }
        if (pid == 0)
        {
            runPeer(opt, region, peerIndex);
            _exit(1);  // unreachable: runPeer always _exit()s
        }
        children.push_back(pid);
    }
    int failures = 0;
    for (const pid_t pid : children)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            ++failures;
        }
    }
    return failures;
}

int runLatencySweep(const Opts_t& opt, Region_t& region)
{
    const auto sweepStart = std::chrono::steady_clock::now();
    const int failures = runPeers(opt, region);
    const auto sweepEnd = std::chrono::steady_clock::now();
    const double wallSec =
        std::chrono::duration_cast<std::chrono::duration<double>>(sweepEnd - sweepStart).count();

    const std::size_t sampleCount = static_cast<std::size_t>(opt.peers) * opt.iters * opt.domains;
    std::vector<std::uint64_t> samples(region.results.data(),
                                       region.results.data() + sampleCount);
    std::sort(samples.begin(), samples.end());
    double sum = 0.0;
    for (auto sample : samples)
    {
        sum += static_cast<double>(sample);
    }
    const double meanNs = samples.empty() ? 0.0 : sum / static_cast<double>(samples.size());
    const double meanUs = meanNs / 1000.0;
    const double p50 = harness::percentile(samples, 0.50) / 1000.0;
    const double p90 = harness::percentile(samples, 0.90) / 1000.0;
    const double p99 = harness::percentile(samples, 0.99) / 1000.0;
    const double tput = wallSec > 0 ? static_cast<double>(samples.size()) / wallSec : 0.0;

    std::printf("\n=== mutex baseline (process-shared pthread_mutex, shm) ===\n");
    std::printf("peers           : %u\n", opt.peers);
    std::printf("domains         : %u\n", opt.domains);
    std::printf("iters (measured): %u (warmup %u)\n", opt.iters, opt.warmup);
    std::printf("acquire latency (us, n=%zu): mean=%.3f p50=%.3f p90=%.3f p99=%.3f\n",
                samples.size(), meanUs, p50, p90, p99);
    std::printf(
        "RESULT peers=%u domains=%u mean_us=%.3f p50_us=%.3f p90_us=%.3f p99_us=%.3f "
        "tput_ops=%.0f samples=%zu\n",
        opt.peers, opt.domains, meanUs, p50, p90, p99, tput, samples.size());

    writeCsvRow(opt, LatencySummary_t{meanUs, p50, p90, p99, samples.size()});
    return failures == 0 ? 0 : 1;
}

int runVerify(const Opts_t& opt, Region_t& region)
{
    const int failures = runPeers(opt, region);

    const std::uint64_t expected =
        static_cast<std::uint64_t>(opt.peers) * static_cast<std::uint64_t>(opt.iters);
    int shortDomains = 0;
    std::printf("VERIFY soak: peers=%u domains=%u iters=%u (expected counter = %" PRIu64
                " per domain)\n",
                opt.peers, opt.domains, opt.iters, expected);
    for (std::uint32_t domainIndex = 0; domainIndex < opt.domains; ++domainIndex)
    {
        const std::uint64_t counted = region.counters[domainIndex];
        if (counted != expected)
        {
            ++shortDomains;
            std::printf("  domain %-3u expected %" PRIu64 " got %" PRIu64 "  (%" PRIu64
                        " LOST updates)\n",
                        domainIndex, expected, counted, expected - counted);
        }
        else if (opt.domains <= 8)
        {
            std::printf("  domain %-3u expected %" PRIu64 " got %" PRIu64 "  OK\n", domainIndex,
                        expected, counted);
        }
    }
    if (shortDomains == 0 && failures == 0)
    {
        std::printf("VERIFY: PASS\n");
        return 0;
    }
    // A peer that died mid-soak also leaves its domains short, so the counter check usually
    // catches it too. Reported separately because a short count alone does not say why.
    std::printf("VERIFY: FAIL (%d/%u domains had lost updates, %d peers exited non-zero)\n",
                shortDomains, opt.domains, failures);
    return 1;
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
    Region_t region = openRegion(ctx, opt);
    if (!region.mapped(opt))
    {
        ctx.check(false, "shared segments mapped");
        return;
    }
    if (!initMutexes(region, opt.domains))
    {
        ctx.check(false, "process-shared robust mutexes initialised");
        return;
    }
    const int exitCode = opt.verify ? runVerify(opt, region) : runLatencySweep(opt, region);
    ctx.check(exitCode == 0, opt.verify ? "verify run" : "latency sweep");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
