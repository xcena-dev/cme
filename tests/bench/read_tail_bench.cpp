// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// read_tail_bench.cpp -- per-access read latency distribution on one 64B line.
//
// Isolates the ORDER handoff "shadow read tail". A UC read that should cost a few
// hundred nanoseconds sometimes costs several microseconds, and there is more than one
// candidate cause. Each mode removes all but one of them, so the four runs together say
// which cause the tail belongs to.
//
//   read-only    Nobody writes the line. This is the floor: media plus interconnect
//                with no contention at all. A tail here is raw CXL jitter.
//   write-read   The timed thread stores to the line, then loads it back. Adds the
//                dirty-line turnaround one thread can cause on its own.
//   concurrent   A second thread stores to the line while this one loads it. Adds true
//                write-read overlap, which is what a real handoff does.
//   flush-read   concurrent, and the reader clflushopts before each load, so every read
//                misses and goes to media. Separates "the cache saved us" from "the
//                medium is slow".
//
// --pollers sweeps pile-up: extra untimed threads spin-loading, to approximate several
// waiters on one controller queue. --stride 0 puts them all on the timed line, and a
// non-zero stride gives each its own. That separates same-line contention from "K more
// reads in flight", which is the shadow-record counterfactual where each waiter polls a
// line of its own.
//
// --target defaults to dax_device from config.yaml, so no device path here belongs to
// one machine. read_tail_bench.sh runs all four modes in order.

#include <immintrin.h>
// clock_gettime, CLOCK_MONOTONIC and nanosleep are POSIX, which <ctime> does not declare.
#include <time.h>  // NOLINT(modernize-deprecated-headers)
#include <x86intrin.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/args.hpp"
#include "helper_util.hpp"
#include "test_memory.hpp"

namespace
{

constexpr std::uint64_t CacheLineBytes = 64;
constexpr std::uint64_t PmdBytes = harness::TestMemory::WindowBytes;
constexpr std::uint64_t DefaultIterations = 2'000'000;
constexpr std::int64_t CalibrationNs = 100'000'000;
constexpr double NsPerSecond = 1e9;
constexpr double OneMicrosecondNs = 1'000.0;
constexpr double FiveMicrosecondsNs = 5'000.0;

constexpr const char* Usage =
    "usage: read_tail_bench [--target <dax-device|file>] [--mode <mode>]\n"
    "                       [--iters N] [--pollers N] [--stride BYTES]\n"
    "  --mode (default read-only):\n"
    "    read-only   no writer at all; the floor for a read on this medium\n"
    "    write-read  this thread stores then loads; dirty-line turnaround\n"
    "    concurrent  a second thread stores while this one loads; write-read overlap\n"
    "    flush-read  concurrent, plus a clflushopt per load, so every read reaches media\n"
    "  --pollers  extra untimed spin-readers, to approximate waiter pile-up\n"
    "  --stride   bytes between poller lines; 0 puts every poller on the timed line\n"
    "  --slot     which 2 MiB window of a devdax node to use; two runs need two slots\n";

// What the timed reader competes with. See the header for what each one isolates.
enum class Mode
{
    ReadOnly,
    WriteRead,
    Concurrent,
    FlushRead,
    Invalid,
};

struct RunOptions_t
{
    std::string target;
    Mode mode{Mode::ReadOnly};
    std::uint64_t iterations{DefaultIterations};
    std::uint32_t pollers{0};
    std::uint64_t stride{0};
    std::uint64_t slot{0};
};

struct Mapping_t
{
    std::uint8_t* base{nullptr};
    std::uint64_t bytes{0};
};

[[nodiscard]] Mode parseMode(const std::string& name)
{
    if (name == "read-only")
    {
        return Mode::ReadOnly;
    }
    if (name == "write-read")
    {
        return Mode::WriteRead;
    }
    if (name == "concurrent")
    {
        return Mode::Concurrent;
    }
    if (name == "flush-read")
    {
        return Mode::FlushRead;
    }
    return Mode::Invalid;
}

[[nodiscard]] const char* getModeName(Mode mode)
{
    switch (mode)
    {
        case Mode::ReadOnly:
            return "read-only";
        case Mode::WriteRead:
            return "write-read";
        case Mode::Concurrent:
            return "concurrent";
        case Mode::FlushRead:
            return "flush-read";
        default:
            return "invalid";
    }
}

// rdtscp, which unlike rdtsc waits for prior loads to retire.
[[nodiscard]] inline std::uint64_t readTscSerialised()
{
    std::uint32_t processorId = 0;
    return __rdtscp(&processorId);
}

// _mm512_set1_epi64 declares its parameter as long long, so the cast has to name that type.
[[nodiscard]] inline __m512i broadcast64(std::uint64_t value)
{
    return _mm512_set1_epi64(static_cast<long long>(value));  // NOLINT(google-runtime-int)
}

// Bytes to map: one PMD unless the poller lines reach past it, which keeps every
// small-stride run at exactly 2 MiB and comparable with every other.
[[nodiscard]] std::uint64_t computeMapBytes(const RunOptions_t& options)
{
    const std::uint64_t needed = options.pollers * options.stride + CacheLineBytes;
    if (needed <= PmdBytes)
    {
        return PmdBytes;
    }
    return ((needed + PmdBytes - 1) / PmdBytes) * PmdBytes;
}

// Divide by the window actually slept, not the one requested: nanosleep returns early on
// a signal and overruns under scheduling delay, and either way biases the rate.
[[nodiscard]] double calibrateTscGhz()
{
    struct timespec before = {};
    struct timespec after = {};
    const struct timespec request = {
        0, CalibrationNs};

    ::clock_gettime(CLOCK_MONOTONIC, &before);
    const std::uint64_t startTsc = readTscSerialised();
    ::nanosleep(&request, nullptr);
    const std::uint64_t endTsc = readTscSerialised();
    ::clock_gettime(CLOCK_MONOTONIC, &after);

    const double windowNs = static_cast<double>(after.tv_sec - before.tv_sec) * NsPerSecond +
                            static_cast<double>(after.tv_nsec - before.tv_nsec);
    if (windowNs <= 0.0)
    {
        return 0.0;
    }
    return static_cast<double>(endTsc - startTsc) / windowNs;
}

void reportResults(const RunOptions_t& options, const Mapping_t& mapping, double tscGhz,
                   std::vector<std::uint32_t>& cycles, std::uint64_t sink)
{
    std::sort(cycles.begin(), cycles.end());
    const std::uint64_t count = cycles.size();

    const auto nsAt = [&](std::uint64_t index)
    {
        return cycles[index] / tscGhz;
    };
    const auto nsAtFraction = [&](double fraction)
    {
        return nsAt(static_cast<std::uint64_t>(static_cast<double>(count) * fraction));
    };

    std::uint64_t overOneUs = 0;
    std::uint64_t overFiveUs = 0;
    for (const std::uint32_t sample : cycles)
    {
        const double sampleNs = sample / tscGhz;
        if (sampleNs > OneMicrosecondNs)
        {
            ++overOneUs;
        }
        if (sampleNs > FiveMicrosecondsNs)
        {
            ++overFiveUs;
        }
    }

    std::printf("path=%s mode=%s iters=%" PRIu64 " pollers=%u stride=%" PRIu64 " map=%" PRIu64
                "MiB tsc=%.3fGHz  (%" PRIu64 ")\n",
                options.target.c_str(), getModeName(options.mode), count, options.pollers,
                options.stride, mapping.bytes >> 20, tscGhz, sink);
    std::printf("  read ns: min=%.0f p50=%.0f p90=%.0f p99=%.1f p999=%.1f p9999=%.1f max=%.1f\n",
                nsAt(0), nsAtFraction(0.5), nsAtFraction(0.9), nsAtFraction(0.99),
                nsAtFraction(0.999), nsAtFraction(0.9999), nsAt(count - 1));
    std::printf("  tail: >1us=%" PRIu64 " (%.3f%%)  >5us=%" PRIu64 " (%.4f%%)\n", overOneUs,
                100.0 * static_cast<double>(overOneUs) / static_cast<double>(count), overFiveUs,
                100.0 * static_cast<double>(overFiveUs) / static_cast<double>(count));
}

}  // namespace

int main(int argc, char** argv)
{
    static_cast<void>(cliargs::takeArgs(argc, argv));

    RunOptions_t options;
    const std::string modeName = cliargs::argStr("--mode", "read-only");
    options.mode = parseMode(modeName);
    if (options.mode == Mode::Invalid)
    {
        std::fprintf(stderr, "%s: unknown --mode '%s'\n%s", argv[0], modeName.c_str(), Usage);
        return 2;
    }

    options.iterations = cliargs::argU64("--iters", DefaultIterations);
    options.pollers = static_cast<std::uint32_t>(cliargs::argU64("--pollers", 0));
    options.stride = cliargs::argU64("--stride", 0);
    options.slot = cliargs::argU64("--slot", 0);

    // Naming, placement and removal come from here. The devdax window sits in the
    // reserved tail rather than at offset 0, where a filesystem sharing the node would be.
    std::unique_ptr<harness::TestMemory> memory;
    try
    {
        memory = harness::TestMemory::open(harness::ConfigReader{}, harness::Backend::Dax,
                                           "read_tail", options.slot,
                                           cliargs::argStr("--target", ""));
    }
    catch (const harness::MediumUnavailable& why)
    {
        std::fprintf(stderr, "%s: %s\n%s", argv[0], why.what(), Usage);
        return 2;
    }
    options.target = memory->name();

    const std::uint64_t mapBytes = computeMapBytes(options);
    const Mapping_t view{static_cast<std::uint8_t*>(memory->map(mapBytes)), mapBytes};
    const Mapping_t* const mapping = &view;
    std::uint8_t* const timedLine = view.base;
    timedLine[0] = 1;  // fault the line in before anything is timed

    const double tscGhz = calibrateTscGhz();
    if (tscGhz <= 0.0)
    {
        std::fprintf(stderr, "tsc calibration failed\n");
        return 1;
    }

    std::atomic<bool> stop{false};

    std::thread writer;
    if (options.mode == Mode::Concurrent || options.mode == Mode::FlushRead)
    {
        writer = std::thread(
            [&]
            {
                std::uint64_t value = 0;
                while (!stop.load(std::memory_order_relaxed))
                {
                    _mm512_storeu_si512(static_cast<void*>(timedLine), broadcast64(value++));
                }
            });
    }

    // Untimed spin readers, approximating several waiters on one controller queue. These
    // are same-host threads, so this is queue pressure rather than true cross-host
    // contention.
    // Touch every poller line from this thread first: the fault belongs to the setup, not to the
    // loop the pollers are about to run.
    for (std::uint32_t index = 0; index < options.pollers; ++index)
    {
        static_cast<void>(*static_cast<volatile std::uint8_t*>(timedLine + (index + 1) * options.stride));
    }

    harness::ThreadGroup pollerThreads;
    pollerThreads.spawn(
        options.pollers,
        [&stop, timedLine, &options](std::uint32_t index)
        {
            std::uint8_t* const pollerLine = timedLine + (index + 1) * options.stride;
            std::uint64_t sink = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                const __m512i loaded = _mm512_loadu_si512(static_cast<const void*>(pollerLine));
                sink += static_cast<std::uint64_t>(_mm512_reduce_add_epi64(loaded));
            }
            static_cast<void>(sink);
        });

    std::vector<std::uint32_t> cycles;
    cycles.reserve(options.iterations);
    std::uint64_t sink = 0;
    std::uint64_t written = 0;
    for (std::uint64_t iteration = 0; iteration < options.iterations; ++iteration)
    {
        if (options.mode == Mode::WriteRead)
        {
            _mm512_storeu_si512(static_cast<void*>(timedLine), broadcast64(written++));
        }
        if (options.mode == Mode::FlushRead)
        {
            _mm_clflushopt(static_cast<void*>(timedLine));
            _mm_mfence();
        }
        // LFENCE on both sides of t0: rdtsc orders prior instructions only, so without the
        // second fence the load can issue before t0 latches, biasing the low tail down.
        _mm_lfence();
        const std::uint64_t startTsc = __rdtsc();
        _mm_lfence();
        const __m512i loaded = _mm512_loadu_si512(static_cast<const void*>(timedLine));
        const std::uint64_t endTsc = readTscSerialised();
        _mm_lfence();
        sink += static_cast<std::uint64_t>(_mm512_reduce_add_epi64(loaded));
        cycles.push_back(static_cast<std::uint32_t>(endTsc - startTsc));
    }

    stop.store(true, std::memory_order_relaxed);
    if (writer.joinable())
    {
        writer.join();
    }
    pollerThreads.join();

    reportResults(options, *mapping, tscGhz, cycles, sink);
    return 0;
}
