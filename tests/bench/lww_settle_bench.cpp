// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// lww_settle_bench.cpp -- empirical settle-time of the atomic-free LWW claim.
//
// The membership claim (src/admission/claim.cpp) stakes a nonce, waits a fixed settle, and
// re-reads, holding the lease iff its nonce survived. This bench asks how long that settle
// needs to be. N threads race from held==0, nothing else touches the line, so the spread is
// staker traffic alone. The settle a staker needs is (last stake - its own stake); the worst
// case in a round is (last stake - first stake), which is what we report.
//
// The nonce lives on a real UC cacheline so stores and loads pay the true cost, and spin
// replaces sleep so scheduler wakeups do not blur the timeline. Caveat: threads on ONE host,
// so this is same-host UC write-settling, not cross-host visibility skew.
//
// This bench sets a constant that ships: ClaimSettle in src/config.hpp. A port to
// different FAM hardware has to re-run it, because too short a settle leaves the
// claim window open and too long only adds latency.
//
// run (an uncacheable line):
//   build/tests/bench/cme-lww-settle-bench --backend uc
//       --contenders "1 2 4 8 16 32 64" --repeats 200
//   (no --backend and no --target uses a plain heap line = WB baseline)
//
// --backend uc resolves file_backend_dir from config.yaml, so no mount path here
// belongs to one machine.
//
// Read settle_p99 and settle_max, not settle_p50. How closely the stakers arrive decides
// whether a round holds a genuine race or a near-miss, and that skew tracks code layout:
// two builds move p50 between 20 ns and 220 ns while p99 and max stay put. A near-miss
// costs nothing to settle, so the median over contended rounds says which mix this build
// happened to sample.

#include <immintrin.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cme/limits.hpp"
#include "common/args.hpp"
#include "common/timing.hpp"
#include "helper_util.hpp"
#include "test_memory.hpp"

namespace
{

constexpr std::uint64_t WordsPerLine = 8;
constexpr std::uint32_t DefaultRepeats = 200;
constexpr std::uint32_t MinContendedStakers = 2;
constexpr double MedianFraction = 0.50;
constexpr double P99Fraction = 0.99;
constexpr std::uint64_t NoStake = std::numeric_limits<std::uint64_t>::max();

constexpr const char* Usage =
    "usage: lww_settle_bench [--backend uc] [--target <file>] [--contenders \"1 2 4\"]\n"
    "                        [--repeats N] [--slot N]\n"
    "  --backend  uc resolves file_backend_dir from config.yaml; omit for a heap WB line\n"
    "  --target   an explicit path, overriding --backend\n"
    "  --slot     which 2 MiB window of a devdax node to use; two runs need two slots\n";

// Pin the calling thread to one core so scheduler preemption cannot stretch the
// stake timeline (the tail we are trying to separate from real UC contention).
void pinTo(std::uint32_t cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(cpu), &set);
    static_cast<void>(::sched_setaffinity(0, sizeof(set), &set));
}

// UC-faithful access to the nonce word: a store fence after write (mirrors
// coherency::wmb), a plain volatile load for the read (UC loads are uncached and
// ordered; an lfence keeps the compiler/CPU from hoisting the read).
void writeNonce(volatile std::uint64_t* word, std::uint64_t nonce)
{
    *word = nonce;
    _mm_sfence();
}

[[nodiscard]] std::uint64_t readNonce(volatile std::uint64_t* word)
{
    _mm_lfence();
    return *word;
}

struct Result_t
{
    std::uint32_t contenders;
    std::uint32_t competed;      // threads that read held==0 and staked
    std::uint64_t firstStakeNs;  // earliest stake, relative to round open
    std::uint64_t lastStakeNs;   // latest stake: from here the winner is fixed
    std::uint64_t settleNeedNs;  // lastStake - firstStake: worst case in this round
    std::uint64_t readSpreadNs;  // last read served - first read served: gate wake skew
    std::uint64_t staleReadNs;   // how long after the first stake a read still returned 0
    std::uint64_t winner;
};

// One round's stamps, every one relative to the origin the opener published, so they share
// one axis.
struct RoundStamps_t
{
    const std::vector<std::uint64_t>& readNs;
    const std::vector<std::uint64_t>& writeNs;
    const std::vector<bool>& staked;
};

// Reduce one round's stamps to the numbers the sweep reports. Split from runOnce so the race
// setup and the arithmetic over its result are read one at a time.
[[nodiscard]] Result_t reduceRound(const RoundStamps_t& stamps, std::uint32_t contenders)
{
    const std::vector<std::uint64_t>& readNs = stamps.readNs;
    const std::vector<std::uint64_t>& writeNs = stamps.writeNs;
    const std::vector<bool>& staked = stamps.staked;

    Result_t result{};
    result.contenders = contenders;

    std::uint64_t firstRead = NoStake;
    std::uint64_t lastRead = 0;
    for (std::uint32_t index = 0; index < contenders; ++index)
    {
        if (readNs[index] < firstRead)
        {
            firstRead = readNs[index];
        }
        if (readNs[index] > lastRead)
        {
            lastRead = readNs[index];
        }
    }
    result.readSpreadNs = (lastRead > firstRead) ? lastRead - firstRead : 0;

    std::uint64_t firstStake = NoStake;
    std::uint64_t lastStake = 0;
    for (std::uint32_t index = 0; index < contenders; ++index)
    {
        if (!staked[index])
        {
            continue;
        }
        ++result.competed;
        if (writeNs[index] < firstStake)
        {
            firstStake = writeNs[index];
        }
        if (writeNs[index] >= lastStake)
        {
            lastStake = writeNs[index];
            result.winner = static_cast<std::uint64_t>(index) + 1;
        }
    }
    result.firstStakeNs = (firstStake == NoStake) ? 0 : firstStake;
    result.lastStakeNs = lastStake;

    // A staker only stakes if its read returned 0. Any staker whose read was served after
    // the first stake was already stamped therefore read a stale 0: that lag is the window
    // in which a late store can still arrive, and it is what the settle has to cover.
    for (std::uint32_t index = 0; index < contenders; ++index)
    {
        if (!staked[index] || readNs[index] <= result.firstStakeNs)
        {
            continue;
        }
        const std::uint64_t lag = readNs[index] - result.firstStakeNs;
        if (lag > result.staleReadNs)
        {
            result.staleReadNs = lag;
        }
    }
    result.settleNeedNs = (lastStake > result.firstStakeNs) ? lastStake - result.firstStakeNs : 0;
    return result;
}

// One race. The round opens through a DRAM gate, not the word, so nothing hot-polls the UC
// line and no reader shares it with the racing stores. Each contender does one UC read:
// 0 -> stake; another's nonce -> it missed the window and waits out the round.
[[nodiscard]] Result_t runOnce(volatile std::uint64_t* nonce, std::uint32_t contenders)
{
    writeNonce(nonce, 0);  // free/unlocked before the round opens

    std::atomic<std::uint32_t> ready{0};     // startup sync only (not the race)
    std::atomic<std::uint64_t> originNs{0};  // common origin, published by the opener
    std::atomic<std::uint32_t> gate{0};      // round-open signal: DRAM (cache-coherent), NOT the FAM
                                             // line -- stakers must not hot-poll the UC nonce to
                                             // learn it started

    std::vector<std::uint64_t> readNs(contenders, 0);   // when staker i's one UC read was served
    std::vector<std::uint64_t> writeNs(contenders, 0);  // when staker i staked (0 = missed the window)
    std::vector<bool> staked(contenders, false);

    // The opener only starts the round -- it never reads the line, so it adds no
    // traffic to the very contention being measured. Stakers report their own stamps.
    std::thread opener(
        [&]
        {
            pinTo(contenders);  // its own core, past the stakers'
            ready.fetch_add(1);
            while (ready.load() < contenders + 1)
            {
                _mm_pause();
            }  // all contenders parked on the gate
            originNs.store(timing::monotonic<timing::Nanos>(), std::memory_order_release);
            gate.store(1, std::memory_order_release);  // OPEN via DRAM gate; nonce is already 0
        });

    harness::runThreads(
        contenders,
        [&](std::uint32_t index)
        {
            const std::uint64_t myNonce = static_cast<std::uint64_t>(index) + 1;  // distinct, nonzero
            pinTo(index);                                                         // one staker per core
            ready.fetch_add(1);
            while (gate.load(std::memory_order_acquire) == 0)
            {
                _mm_pause();
            }  // DRAM spin, no FAM poll
            const std::uint64_t origin = originNs.load(std::memory_order_acquire);
            const std::uint64_t seen = readNonce(nonce);  // one UC read: free(0)? then stake
            readNs[index] = timing::monotonic<timing::Nanos>() - origin;
            if (seen == 0)  // saw it free -> stake (claim.cpp case 1)
            {
                writeNonce(nonce, myNonce);
                writeNs[index] = timing::monotonic<timing::Nanos>() - origin;
                staked[index] = true;
            }
            // else: door already closed; wait for next round (no re-contend).
        });
    opener.join();

    return reduceRound({readNs, writeNs, staked}, contenders);
}

// Thread counts from a space- or comma-separated list, e.g. "1 2 4 8".
[[nodiscard]] std::vector<std::uint32_t> parseCounts(const std::string& list)
{
    std::vector<std::uint32_t> counts;
    std::string token;
    for (std::uint64_t cursor = 0; cursor <= list.size(); ++cursor)
    {
        const bool atEnd = (cursor == list.size());
        const char letter = atEnd ? '\0' : list[cursor];
        if (atEnd || letter == ' ' || letter == ',')
        {
            if (!token.empty())
            {
                counts.push_back(static_cast<std::uint32_t>(std::stoul(token)));
                token.clear();
            }
            continue;
        }
        token.push_back(letter);
    }
    return counts;
}

// Value at @fraction of a sorted sample, which the caller has already sorted.
[[nodiscard]] std::uint64_t percentileOf(const std::vector<std::uint64_t>& sorted, double fraction)
{
    if (sorted.empty())
    {
        return 0;
    }
    const auto index = static_cast<std::uint64_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

[[nodiscard]] double meanOf(const std::vector<double>& samples)
{
    if (samples.empty())
    {
        return 0.0;
    }
    double sum = 0.0;
    for (const double sample : samples)
    {
        sum += sample;
    }
    return sum / static_cast<double>(samples.size());
}

}  // namespace

int main(int argc, char** argv)
{
    static_cast<void>(cliargs::takeArgs(argc, argv));

    // No backend is the heap WB baseline, so an absent one is not an error.
    const std::string backend = cliargs::argStr("--backend", std::string{});
    if (!backend.empty() && backend != "uc")
    {
        std::fprintf(stderr, "%s: unknown backend '%s' (only uc; omit for the heap baseline)\n%s",
                     argv[0], backend.c_str(), Usage);
        return 2;
    }

    const std::vector<std::uint32_t> contenders = parseCounts(cliargs::argStr("--contenders", "1 2 3 4"));
    if (contenders.empty())
    {
        std::fprintf(stderr, "%s: --contenders parsed to nothing\n%s", argv[0], Usage);
        return 2;
    }
    const auto repeats = static_cast<std::uint32_t>(cliargs::argU64("--repeats", DefaultRepeats));
    const std::uint64_t slot = cliargs::argU64("--slot", 0);

    // Map one cacheline: the uncacheable file when a backend was selected, else a heap line
    // as the write-back baseline.
    static std::uint64_t heapLine[WordsPerLine] __attribute__((aligned(cme::CacheLineBytes))) = {0};
    volatile std::uint64_t* nonce = heapLine;
    std::unique_ptr<harness::TestMemory> memory;
    if (!backend.empty())
    {
        try
        {
            memory = harness::TestMemory::open(harness::ConfigReader{},
                                               harness::backendFromName(backend), "lww_probe",
                                               slot, cliargs::argStr("--target", ""));
        }
        catch (const harness::MediumUnavailable& why)
        {
            std::fprintf(stderr, "%s: %s\n%s", argv[0], why.what(), Usage);
            return 2;
        }
        nonce = static_cast<volatile std::uint64_t*>(
            memory->map(harness::TestMemory::WindowBytes));
    }
    const std::string target = (memory != nullptr) ? memory->name() : std::string{};

    std::printf("# LWW claim settle bench  (backend=%s, repeats=%u)\n",
                target.empty() ? "heap-WB" : target.c_str(), repeats);
    std::printf("# settle need = last stake - first stake (ns); no reader shares the line during a round\n");
    std::printf("%-4s %-9s %-9s %-13s %-13s %-13s %-13s\n", "N", "competed", "contended",
                "settle_p50", "settle_p99", "settle_max", "stale0_p50");

    for (const std::uint32_t count : contenders)
    {
        // Percentiles over *contended* rounds only. Most rounds have a single staker
        // (the rest miss the 0 window), and those need no settle at all -- mixing them
        // in would report how often two stakers race, not how long a race takes.
        std::vector<std::uint64_t> need;
        std::vector<std::uint64_t> stale;
        std::vector<double> competed;
        for (std::uint32_t round = 0; round < repeats; ++round)
        {
            const Result_t result = runOnce(nonce, count);
            if (result.competed >= MinContendedStakers)
            {
                need.push_back(result.settleNeedNs);
                stale.push_back(result.staleReadNs);
            }
            competed.push_back(result.competed);
        }

        const std::uint64_t contendedRounds = need.size();
        if (need.empty())
        {
            need.push_back(0);
        }
        std::sort(stale.begin(), stale.end());
        std::sort(need.begin(), need.end());

        std::printf("%-4u %-9.1f %-9" PRIu64 " %-13" PRIu64 " %-13" PRIu64 " %-13" PRIu64
                    " %-13" PRIu64 "\n",
                    count, meanOf(competed), contendedRounds, percentileOf(need, MedianFraction),
                    percentileOf(need, P99Fraction), need.back(),
                    percentileOf(stale, MedianFraction));
    }

    return 0;  // the area goes away with memory_, whichever backend it was on
}
