// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cacheline_bench.cpp -- ns/iter for load and store on one 64B line.
//
// Two mappings over the same CXL media, so the pair isolates the cache regime
// rather than the medium:
//   dax : devdax raw, write-back cached; stores need clwb+sfence to reach media
//   uc  : a file on an uncacheable mount; stores go straight through, no flush
//
// Both targets come from config.yaml (dax_device, file_backend_dir), so no path
// here belongs to one machine.
//
//   ./cacheline_bench --backend dax
//   ./cacheline_bench --backend uc
#include <immintrin.h>
// clock_gettime and CLOCK_MONOTONIC are POSIX, which <ctime> does not declare.
#include <time.h>  // NOLINT(modernize-deprecated-headers)

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "common/args.hpp"
#include "test_memory.hpp"

namespace
{

constexpr std::uint64_t DefaultIterations = 200'000;

// Warm-up length as a fraction of the timed run: enough to fault the line in and settle
// the branch predictors, short enough not to double the run.
constexpr std::uint64_t WarmupDivisor = 20;

constexpr std::uint64_t HalfLineBytes = 32;
constexpr std::uint32_t WordsPerLine = 8;
constexpr double NsPerSecond = 1e9;

constexpr const char* Usage =
    "usage: cacheline_bench [--backend dax|uc] [--target <dax-device|file>]\n"
    "                       [--iters N] [--slot N]\n"
    "  --backend  resolves the target from config.yaml (default dax)\n"
    "  --target   an explicit path, overriding --backend\n"
    "  --slot     which 2 MiB window of a devdax node to use; two runs need two slots\n";

// Kept live so the compiler cannot drop a load whose result nothing else reads.
volatile std::uint64_t g_sink = 0;

[[nodiscard]] double readClockNs()
{
    struct timespec now = {};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) * NsPerSecond + static_cast<double>(now.tv_nsec);
}

void reportRate(const char* label, double elapsedNs, std::uint64_t iterations)
{
    std::printf("  %-20s %8.1f ns/iter\n", label, elapsedNs / static_cast<double>(iterations));
}

// Times @load, which reads the line and folds what it read into the accumulator it is
// handed. The clobber each iteration forces a re-read rather than a hoisted one, and the
// escape after the loop keeps the accumulator from being dead code.
template <typename T_Load>
void timeLoad(const char* label, std::uint64_t iterations, T_Load load)
{
    std::uint64_t accumulator = 0;
    for (std::uint64_t warmup = 0; warmup < iterations / WarmupDivisor; ++warmup)
    {
        asm volatile("" ::: "memory");
        accumulator = load(accumulator);
    }

    accumulator = 0;
    const double startNs = readClockNs();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration)
    {
        asm volatile("" ::: "memory");
        accumulator = load(accumulator);
    }
    const double endNs = readClockNs();

    asm volatile("" : : "r"(accumulator) : "memory");
    g_sink = accumulator;
    reportRate(label, endNs - startNs, iterations);
}

// Times @store, which writes the line and returns the value the next iteration should
// write. Passing the value through rather than capturing it keeps it in a register
// whether or not this template is inlined. The clobbers on both sides stop the write
// being reordered out of the timed window.
template <typename T_Store>
void timeStore(const char* label, std::uint64_t iterations, T_Store store)
{
    std::uint64_t value = 0;
    for (std::uint64_t warmup = 0; warmup < iterations / WarmupDivisor; ++warmup)
    {
        asm volatile("" ::: "memory");
        value = store(value);
        asm volatile("" ::: "memory");
    }

    const double startNs = readClockNs();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration)
    {
        asm volatile("" ::: "memory");
        value = store(value);
        asm volatile("" ::: "memory");
    }
    const double endNs = readClockNs();

    g_sink = value;
    reportRate(label, endNs - startNs, iterations);
}

void runLoads(std::uint8_t* line, std::uint64_t iterations)
{
    std::printf("-- LOAD --\n");

    timeLoad("scalar 8Bx8", iterations,
             [line](std::uint64_t accumulator)
             {
                 const auto* words = reinterpret_cast<const volatile std::uint64_t*>(line);
                 for (std::uint32_t index = 0; index < WordsPerLine; ++index)
                 {
                     accumulator += words[index];
                 }
                 return accumulator;
             });

    // The AVX512 intrinsics are always_inline, so a target without AVX512F fails to compile
    // rather than warn. main() prints which probes the build left out.
#ifdef __AVX512F__
    timeLoad("AVX512 64Bx1", iterations,
             [line](std::uint64_t accumulator)
             {
                 const __m512i loaded = _mm512_loadu_si512(static_cast<const void*>(line));
                 return accumulator + static_cast<std::uint64_t>(_mm512_reduce_add_epi64(loaded));
             });
#endif

#ifdef __AVX2__
    timeLoad("MOVNTDQA 32Bx2", iterations,
             [line](std::uint64_t accumulator)
             {
                 const __m256i low =
                     _mm256_stream_load_si256(reinterpret_cast<const __m256i*>(line));
                 const __m256i high = _mm256_stream_load_si256(
                     reinterpret_cast<const __m256i*>(line + HalfLineBytes));
                 const __m256i folded = _mm256_xor_si256(low, high);
                 return accumulator + static_cast<std::uint64_t>(_mm256_extract_epi64(folded, 0));
             });
#endif

    // Invalidate before the load, so it refills from media. That is the coherency-less FAM
    // read on WB; on UC it is redundant, since nothing was cached to begin with.
#ifdef __AVX512F__
    timeLoad("clflush then load", iterations,
             [line](std::uint64_t accumulator)
             {
                 _mm_clflushopt(static_cast<void*>(line));
                 _mm_mfence();
                 const __m512i loaded = _mm512_loadu_si512(static_cast<const void*>(line));
                 return accumulator + static_cast<std::uint64_t>(_mm512_reduce_add_epi64(loaded));
             });
#endif
}

#ifdef __AVX512F__
// _mm512_set1_epi64 declares its parameter as long long, so the cast has to name that type.
__m512i broadcast64(std::uint64_t value)
{
    return _mm512_set1_epi64(static_cast<long long>(value));  // NOLINT(google-runtime-int)
}
#endif

void runStores(std::uint8_t* line, std::uint64_t iterations)
{
    std::printf("-- STORE --\n");

    timeStore("scalar 8Bx8", iterations,
              [line](std::uint64_t value)
              {
                  auto* words = reinterpret_cast<volatile std::uint64_t*>(line);
                  for (std::uint32_t index = 0; index < WordsPerLine; ++index)
                  {
                      words[index] = value;
                  }
                  return value + 1;
              });

#ifdef __AVX512F__
    timeStore("AVX512 64Bx1", iterations,
              [line](std::uint64_t value)
              {
                  _mm512_storeu_si512(static_cast<void*>(line), broadcast64(value));
                  return value + 1;
              });

    timeStore("AVX512 + sfence", iterations,
              [line](std::uint64_t value)
              {
                  _mm512_storeu_si512(static_cast<void*>(line), broadcast64(value));
                  _mm_sfence();
                  return value + 1;
              });

    timeStore("AVX512 + clwb+sfence", iterations,
              [line](std::uint64_t value)
              {
                  _mm512_storeu_si512(static_cast<void*>(line), broadcast64(value));
                  _mm_clwb(static_cast<void*>(line));
                  _mm_sfence();
                  return value + 1;
              });

    timeStore("AVX512 + clflushopt", iterations,
              [line](std::uint64_t value)
              {
                  _mm512_storeu_si512(static_cast<void*>(line), broadcast64(value));
                  _mm_clflushopt(static_cast<void*>(line));
                  _mm_sfence();
                  return value + 1;
              });

    timeStore("NT 64B + sfence", iterations,
              [line](std::uint64_t value)
              {
                  _mm512_stream_si512(reinterpret_cast<__m512i*>(line), broadcast64(value));
                  _mm_sfence();
                  return value + 1;
              });
#endif
}

}  // namespace

int main(int argc, char** argv)
{
    static_cast<void>(cliargs::takeArgs(argc, argv));

    const std::string backend = cliargs::argStr("--backend", "dax");
    if (backend != "dax" && backend != "uc")
    {
        std::fprintf(stderr, "%s: unknown backend '%s' (want dax or uc)\n%s", argv[0],
                     backend.c_str(), Usage);
        return 2;
    }

    const std::uint64_t iterations = cliargs::argU64("--iters", DefaultIterations);
    const std::uint64_t slot = cliargs::argU64("--slot", 0);

    // Naming, placement and removal all come from here. On dax that placement matters: the
    // node may carry a filesystem at its front, so the window sits in the reserved tail
    // rather than at offset 0.
    std::unique_ptr<harness::TestMemory> memory;
    try
    {
        memory = harness::TestMemory::open(harness::ConfigReader{}, harness::backendFromName(backend),
                                           "cl_bench", slot, cliargs::argStr("--target", ""));
    }
    catch (const harness::MediumUnavailable& why)
    {
        std::fprintf(stderr, "%s: %s\n%s", argv[0], why.what(), Usage);
        return 2;
    }

    auto* const line = static_cast<std::uint8_t*>(memory->map(harness::TestMemory::WindowBytes));
    line[0] = 1;  // fault the line in before anything is timed

    std::printf("path=%s  mapping=%s  iters=%" PRIu64 " (one 64B line, repeated)\n",
                memory->name().c_str(), backend == "dax" ? "WB(devdax)" : "UC(file)", iterations);
#ifndef __AVX512F__
    std::printf("note: no AVX512F in this build, so the 512-bit probes are absent\n");
#endif
#ifndef __AVX2__
    std::printf("note: no AVX2 in this build, so the MOVNTDQA probe is absent\n");
#endif

    runLoads(line, iterations);
    runStores(line, iterations);
    return 0;
}
