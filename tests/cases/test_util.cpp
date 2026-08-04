// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_util.cpp -- exhaustive tests for util/endian.hpp + util/coherency.hpp. Covers:
//   - endian::load / store round-trip on all scalar widths
//   - Field_t<T> assign / convert / copy / adjacency / sizeof / alignof
//   - Tear-free concurrent access on aligned scalar
//   - Cross-thread happens-before via wmb/rmb (writer sets data + flag,
//     reader observes flag → must see data)
//   - Two-word snapshot torn-pair stress (mirrors getOwnershipSnapshot vulnerability)
//   - Multi-writer / multi-reader hammer on a single Field_t<u64>
//   - Cross-process shared-memory tests (fork + MAP_SHARED|MAP_ANONYMOUS):
//       Field_t<T> visibility across processes
//       wmb/rmb happens-before across processes

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "cme/shared.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"
#include "util/endian.hpp"

namespace
{

// ── endian::load / endian::store round-trip ───────────────────────

template <typename T>
void roundTripScalar(harness::TestContext& ctx, const char* tag)
{
    T storage{};
    constexpr T Pattern = static_cast<T>(0xA5A5A5A5A5A5A5A5ULL);
    cme::endian::store(storage, Pattern);
    const T back = cme::endian::load(storage);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "endian round-trip (%s)", tag);
    ctx.check(back == Pattern, buf);
}

void testEndianScalars(harness::TestContext& ctx)
{
    std::printf("== endian::load / store round-trip\n");
    roundTripScalar<std::uint8_t>(ctx, "u8");
    roundTripScalar<std::uint16_t>(ctx, "u16");
    roundTripScalar<std::uint32_t>(ctx, "u32");
    roundTripScalar<std::uint64_t>(ctx, "u64");
}

// ── Field_t<T> wrapper semantics ────────────────────────────────────

template <typename T>
void fieldRoundTrip(harness::TestContext& ctx, const char* tag)
{
    using F = cme::endian::Field_t<T>;
    static_assert(sizeof(F) == sizeof(T), "Field_t<T> size must match T");
    static_assert(alignof(F) == alignof(T), "Field_t<T> align must match T");

    F field{};
    constexpr T Pattern = static_cast<T>(0x5A5A5A5A5A5A5A5AULL);
    field = Pattern;
    const T back = field;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Field_t<%s> assign + convert round-trip", tag);
    ctx.check(back == Pattern, buf);

    const F copy = field;
    std::snprintf(buf, sizeof(buf), "Field_t<%s> copy preserves value", tag);
    ctx.check(static_cast<T>(copy) == Pattern, buf);

    F assigned{};
    assigned = copy;
    std::snprintf(buf, sizeof(buf), "Field_t<%s> Field_t=Field_t preserves value", tag);
    ctx.check(static_cast<T>(assigned) == Pattern, buf);
}

void testFieldRoundTrip(harness::TestContext& ctx)
{
    std::printf("== Field_t<T> assign/convert/copy\n");
    fieldRoundTrip<std::uint8_t>(ctx, "u8");
    fieldRoundTrip<std::uint16_t>(ctx, "u16");
    fieldRoundTrip<std::uint32_t>(ctx, "u32");
    fieldRoundTrip<std::uint64_t>(ctx, "u64");
}

struct AdjacencyProbe_t
{
    cme::endian::Field_t<std::uint32_t> a;
    cme::endian::Field_t<std::uint64_t> b;
    cme::endian::Field_t<std::uint32_t> c;
};

void testFieldAdjacency(harness::TestContext& ctx)
{
    std::printf("== Field_t<T> adjacency / no stomp\n");
    AdjacencyProbe_t probe{};
    probe.a = 0xAAAAAAAAu;
    probe.b = 0xBBBBBBBBBBBBBBBBULL;
    probe.c = 0xCCCCCCCCu;
    ctx.check(probe.a == 0xAAAAAAAAu, "a unchanged after b+c writes");
    ctx.check(probe.b == 0xBBBBBBBBBBBBBBBBULL, "b unchanged after c write");
    ctx.check(probe.c == 0xCCCCCCCCu, "c written");
}

// ── Concurrent tear-free access on aligned u64 Field_t ───────────────

void testTearFreeU64(harness::TestContext& ctx)
{
    std::printf("== Field_t<u64> tear-free under concurrent rw\n");
    constexpr std::uint64_t PatternA = 0x1111111111111111ULL;
    constexpr std::uint64_t PatternB = 0xEEEEEEEEEEEEEEEEULL;

    struct Holder_t
    {
        cme::endian::Field_t<std::uint64_t> slot;
    } holder{};
    holder.slot = PatternA;

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint64_t> samples{0};

    std::thread writer{[&]
                       {
                           std::uint64_t round = 0;
                           while (!stop.load(std::memory_order_relaxed))
                           {
                               holder.slot = (round++ & 1u) ? PatternA : PatternB;
                           }
                       }};

    std::thread reader{[&]
                       {
                           for (std::int32_t round = 0; round < 2'000'000; ++round)
                           {
                               const std::uint64_t sample = holder.slot;
                               samples.fetch_add(1, std::memory_order_relaxed);
                               if (sample != PatternA && sample != PatternB)
                               {
                                   torn.fetch_add(1, std::memory_order_relaxed);
                               }
                           }
                           stop.store(true, std::memory_order_relaxed);
                       }};

    reader.join();
    writer.join();

    char buf[96];
    std::snprintf(buf, sizeof(buf), "no torn samples (%" PRIu64 " observed)", samples.load());
    ctx.check(torn.load() == 0, buf);
}

// ── Cross-thread happens-before via wmb / rmb ─────────────────────
//
// Writer stages the payload, sets ready=1, wmb. A reader seeing ready==1 after rmb MUST see
// the staged payload. Impossible to violate on TSO x86, but it pins the API contract.

struct HappensBeforeBlock_t
{
    cme::endian::Field_t<std::uint64_t> payload;
    cme::endian::Field_t<std::uint32_t> ready;
};

constexpr std::int32_t HappensBeforeIters = 50'000;
constexpr std::uint64_t HappensBeforeMix = 0x9E3779B97F4A7C15ULL;

// What the two threads share. ready carries the iteration number, seen reports the reader's
// progress back to the writer, and done ends the run from whichever side finishes first.
struct HappensBeforeRun_t
{
    HappensBeforeBlock_t block{};
    std::atomic<std::int32_t> mismatch{0};
    std::atomic<std::int32_t> seen{0};
    std::atomic<bool> done{false};
};

// Stages the payload, then publishes the iteration number, with a wmb after each store.
void publishHappensBefore(HappensBeforeRun_t& run, cme::CoherencyMode coherency)
{
    for (std::int32_t i = 1; i <= HappensBeforeIters; ++i)
    {
        run.block.ready = 0u;
        cme::coherency::wmb(&run.block, sizeof(run.block), coherency);
        run.block.payload = static_cast<std::uint64_t>(i) * HappensBeforeMix;
        cme::coherency::wmb(&run.block.payload, sizeof(run.block.payload), coherency);
        run.block.ready = static_cast<std::uint32_t>(i);
        cme::coherency::wmb(&run.block.ready, sizeof(run.block.ready), coherency);
        // hold this iteration until the reader reports it, so no publication goes unread
        while (!run.done.load(std::memory_order_acquire) && run.block.ready != 0u &&
               run.seen.load(std::memory_order_acquire) < i)
        {
            std::this_thread::yield();
        }
        if (run.done.load(std::memory_order_acquire))
        {
            break;
        }
    }
    run.done.store(true, std::memory_order_release);
}

// Reads the iteration number, then the payload, with an rmb before each. A payload that does
// not match the number it was published with is a happens-before violation.
void observeHappensBefore(HappensBeforeRun_t& run, cme::CoherencyMode coherency)
{
    std::int32_t last = 0;
    while (!run.done.load(std::memory_order_acquire))
    {
        cme::coherency::rmb(&run.block.ready, sizeof(run.block.ready), coherency);
        const std::uint32_t readyVal = run.block.ready;
        if (readyVal == 0 || static_cast<std::int32_t>(readyVal) == last)
        {
            continue;
        }
        cme::coherency::rmb(&run.block.payload, sizeof(run.block.payload), coherency);
        const std::uint64_t got = run.block.payload;
        if (got != static_cast<std::uint64_t>(readyVal) * HappensBeforeMix)
        {
            run.mismatch.fetch_add(1, std::memory_order_relaxed);
        }
        last = static_cast<std::int32_t>(readyVal);
        run.seen.store(last, std::memory_order_release);
        if (last >= HappensBeforeIters)
        {
            break;
        }
    }
    run.done.store(true, std::memory_order_release);
}

void testHappensBefore(harness::TestContext& ctx)
{
    std::printf("== wmb/rmb happens-before across threads\n");
    HappensBeforeRun_t run;

    std::thread writer{[&]
                       {
                           publishHappensBefore(run, ctx.coherency());
                       }};
    std::thread reader{[&]
                       {
                           observeHappensBefore(run, ctx.coherency());
                       }};

    writer.join();
    reader.join();

    char buf[96];
    std::snprintf(buf, sizeof(buf), "ready visible → payload visible (last seen=%d, mismatches=%d)",
                  run.seen.load(), run.mismatch.load());
    ctx.check(run.mismatch.load() == 0, buf);
}

// ── Torn pair stress (mirrors getOwnershipSnapshot) ──────────────────────
//
// Ownership-like layout: holder (u32) + generation (u64), rotated together under wmb. A
// naive 2-word reader can see (newHolder, oldGen) on weakly-ordered hardware; on x86 TSO it
// should not, but only if the compiler keeps the loads in program order.

struct OwnershipLike_t
{
    cme::endian::Field_t<std::uint32_t> holder;
    cme::endian::Field_t<std::uint32_t> pad;
    cme::endian::Field_t<std::uint64_t> gen;
};

constexpr std::int32_t OwnershipSamples = 1'000'000;

// The pair under test plus the counters its two threads share.
struct OwnershipRun_t
{
    OwnershipLike_t ownership{};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint64_t> samples{0};
};

// Rotates holder and gen to the same value, so a reader that sees holder != gen read the pair
// mid-rotation.
void rotateOwnership(OwnershipRun_t& run, cme::CoherencyMode coherency)
{
    std::uint64_t round = 0;
    while (!run.stop.load(std::memory_order_relaxed))
    {
        ++round;
        run.ownership.holder = static_cast<std::uint32_t>(round & 0xFFFFu);
        run.ownership.gen = round & 0xFFFFu;
        cme::coherency::wmb(&run.ownership, sizeof(run.ownership), coherency);
    }
}

// Reads holder and gen as two separate words, which is the pattern under test.
void sampleOwnership(OwnershipRun_t& run, cme::CoherencyMode coherency)
{
    for (std::int32_t round = 0; round < OwnershipSamples; ++round)
    {
        cme::coherency::rmb(&run.ownership, sizeof(run.ownership), coherency);
        const std::uint32_t holder = run.ownership.holder;
        const std::uint64_t gen = run.ownership.gen;
        run.samples.fetch_add(1, std::memory_order_relaxed);
        if (static_cast<std::uint64_t>(holder) != gen)
        {
            run.torn.fetch_add(1, std::memory_order_relaxed);
        }
    }
    run.stop.store(true, std::memory_order_relaxed);
}

void testOwnershipPairSnapshot(harness::TestContext& ctx)
{
    std::printf("== Ownership (holder,gen) two-word snapshot consistency\n");
    OwnershipRun_t run;
    run.ownership.holder = 0u;
    run.ownership.gen = 0ULL;

    std::thread writer{[&]
                       {
                           rotateOwnership(run, ctx.coherency());
                       }};
    std::thread reader{[&]
                       {
                           sampleOwnership(run, ctx.coherency());
                       }};

    reader.join();
    writer.join();

    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "two-word read torn samples = %" PRIu64 " / %" PRIu64
                  " (NOTE: race expected; rmb does not make a pair atomic)",
                  run.torn.load(), run.samples.load());
    // This is an informational test, not a strict check -- it documents
    // the vulnerability. Pass criterion: test completes without crash.
    ctx.check(run.samples.load() > 0, buf);
}

// ── Multi-writer / multi-reader hammer on one Field_t<u64> ──────────────────
//
// The writers only ever store one of these, so a reader that sees anything else read a word
// while it was half written. At namespace scope because both thread bodies need them.
constexpr std::uint64_t HammerValues[] = {0x1111111111111111ULL, 0x2222222222222222ULL,
                                          0x3333333333333333ULL, 0x4444444444444444ULL,
                                          0x5555555555555555ULL, 0x6666666666666666ULL,
                                          0x7777777777777777ULL, 0x8888888888888888ULL};
constexpr std::size_t HammerValueCount = sizeof(HammerValues) / sizeof(HammerValues[0]);

// The one slot under test plus the counters its threads share.
struct HammerState_t
{
    cme::endian::Field_t<std::uint64_t> slot;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0};
};

// Store values into the slot until the run is stopped. @writerIndex offsets the starting
// value so the writers are not in lockstep, which is what makes a tear reachable.
void hammerWrite(HammerState_t& state, std::int32_t writerIndex) noexcept
{
    auto next = static_cast<std::uint64_t>(writerIndex);
    while (!state.stop.load(std::memory_order_relaxed))
    {
        state.slot = HammerValues[next++ % HammerValueCount];
    }
}

// Sample the slot @samples times; count every value that is not one a writer could have
// stored.
void hammerRead(HammerState_t& state, std::int32_t samples) noexcept
{
    for (std::int32_t taken = 0; taken < samples; ++taken)
    {
        const std::uint64_t sample = state.slot;
        bool known = false;
        for (const std::uint64_t candidate : HammerValues)
        {
            if (sample == candidate)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            state.torn.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void testHammerU64(harness::TestContext& ctx)
{
    std::printf("== Field_t<u64> multi-writer / multi-reader hammer\n");
    constexpr std::int32_t Writers = 4;
    constexpr std::int32_t Readers = 4;
    constexpr std::int32_t SamplesPerReader = 250'000;

    HammerState_t state{};
    state.slot = HammerValues[0];

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(Writers) + static_cast<std::size_t>(Readers));
    for (std::int32_t writer = 0; writer < Writers; ++writer)
    {
        threads.emplace_back(hammerWrite, std::ref(state), writer);
    }
    for (std::int32_t reader = 0; reader < Readers; ++reader)
    {
        threads.emplace_back(hammerRead, std::ref(state), SamplesPerReader);
    }
    // give readers time to start, then stop
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    state.stop.store(true, std::memory_order_relaxed);
    for (auto& thread : threads)
    {
        thread.join();
    }

    char buf[96];
    std::snprintf(buf, sizeof(buf), "no torn samples (torn=%" PRIu64 ")", state.torn.load());
    ctx.check(state.torn.load() == 0, buf);
}

// ── coherency::wmb / rmb basic invocation ─────────────────────────

void testCoherencyFences(harness::TestContext& ctx)
{
    std::printf("== coherency::wmb / rmb basic invocation\n");
    alignas(64) std::uint64_t buf[8]{};
    cme::coherency::wmb(buf, sizeof(buf), ctx.coherency());
    cme::coherency::rmb(buf, sizeof(buf), ctx.coherency());
    cme::coherency::wmb(buf, 1, ctx.coherency());
    cme::coherency::rmb(buf, 1, ctx.coherency());
    ctx.check(true, "wmb/rmb invoke without crash");
}

// ── Cross-process tests via fork + MAP_SHARED|MAP_ANONYMOUS ────────

struct XpBlock_t
{
    cme::endian::Field_t<std::uint64_t> payload;
    cme::endian::Field_t<std::uint32_t> ready;
    cme::endian::Field_t<std::uint32_t> pad;
};

void testCrossProcessVisibility(harness::TestContext& ctx)
{
    std::printf("== cross-process Field_t<T> visibility (fork + MAP_SHARED)\n");
    const auto block = ctx.scratch<XpBlock_t>("visibility", 1);
    auto* blk = block.data();
    if (!block)
    {
        ctx.check(false, "shared record mapped");
        return;
    }

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        std::perror("fork");
        ctx.check(false, "fork failed");
        return;
    }
    if (pid == 0)
    {
        // Child: wait until parent sets payload + ready.
        constexpr std::uint64_t Expected = 0xDEADBEEFCAFEBABEULL;
        std::int32_t mismatch = 0;
        for (std::int32_t spin = 0; spin < 50'000'000; ++spin)
        {
            cme::coherency::rmb(&blk->ready, sizeof(blk->ready), ctx.coherency());
            if (blk->ready != 0u)
            {
                cme::coherency::rmb(&blk->payload, sizeof(blk->payload), ctx.coherency());
                if (blk->payload != Expected)
                {
                    mismatch = 1;
                }
                std::_Exit(mismatch);
            }
        }
        std::_Exit(99);  // timeout marker
    }

    // Parent: stage payload + ready.
    blk->payload = 0xDEADBEEFCAFEBABEULL;
    cme::coherency::wmb(&blk->payload, sizeof(blk->payload), ctx.coherency());
    blk->ready = 1u;
    cme::coherency::wmb(&blk->ready, sizeof(blk->ready), ctx.coherency());

    int status = 0;
    ::waitpid(pid, &status, 0);
    const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    ctx.check(exitCode == 0, "child observed payload after seeing ready (rc=0 means match)");
}

void testCrossProcessHammer(harness::TestContext& ctx)
{
    std::printf("== cross-process Field_t<u64> hammer (no tear)\n");
    const auto block = ctx.scratch<XpBlock_t>("hammer", 1);
    auto* blk = block.data();
    if (!block)
    {
        ctx.check(false, "shared record mapped");
        return;
    }
    blk->payload = 0x1111111111111111ULL;

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        std::perror("fork");
        ctx.check(false, "fork failed");
        return;
    }
    if (pid == 0)
    {
        // Child = writer cycling PatternA <-> PatternB
        constexpr std::uint64_t PatternA = 0x1111111111111111ULL;
        constexpr std::uint64_t PatternB = 0xEEEEEEEEEEEEEEEEULL;
        for (std::int32_t round = 0; round < 5'000'000; ++round)
        {
            blk->payload = (round & 1) ? PatternA : PatternB;
        }
        std::_Exit(0);
    }
    // Parent = reader
    constexpr std::uint64_t PatternA = 0x1111111111111111ULL;
    constexpr std::uint64_t PatternB = 0xEEEEEEEEEEEEEEEEULL;
    std::uint64_t torn = 0;
    for (std::int32_t round = 0; round < 5'000'000; ++round)
    {
        const std::uint64_t sample = blk->payload;
        if (sample != PatternA && sample != PatternB)
        {
            ++torn;
        }
    }
    int status = 0;
    ::waitpid(pid, &status, 0);

    char buf[96];
    std::snprintf(buf, sizeof(buf), "no torn cross-process samples (torn=%" PRIu64 ")", torn);
    ctx.check(torn == 0, buf);
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    testEndianScalars(ctx);
    testFieldRoundTrip(ctx);
    testFieldAdjacency(ctx);
    testTearFreeU64(ctx);
    testHappensBefore(ctx);
    testOwnershipPairSnapshot(ctx);
    testHammerU64(ctx);
    testCoherencyFences(ctx);
    testCrossProcessVisibility(ctx);
    testCrossProcessHammer(ctx);
}

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, runBody);
}
