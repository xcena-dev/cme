// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_shared_session.cpp -- SharedSession's contract: one peer, many threads, still exclusive.
//
// The inverse of test_mutual_exclusion, which gives every thread its own Peer to stand in for a
// remote one. Here ONE session is shared by N threads of this process, which is the case cme's
// per-peer ownership token does not cover on its own: a second thread of the same peer takes the
// domain lock immediately on the resident fast path. A non-atomic RMW inside the critical section
// turns any overlap into a lost update, so an exact total is the evidence the intra-node tier ran.
//
// Run twice: with cohorting at its default cap, and with the cap at 1 (every acquire does a full
// release/re-acquire), since those are separate paths through SharedSession::lock.

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include "cme/shared_session.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr std::uint32_t Threads = 8;
constexpr std::uint32_t ItersPerThread = 2000;
constexpr const char* Domain = "lane0";

// Non-atomic on purpose: the lock is what must serialise the RMW. Two threads inside the
// critical section at once lose an increment, so the total falls short.
std::uint64_t g_counter = 0;

// Returns the number of threads that threw.
std::uint32_t hammer(cme::SharedSession& shared)
{
    g_counter = 0;
    std::atomic<std::uint32_t> failures{0};

    harness::runThreads(
        Threads,
        [&shared, &failures](std::uint32_t)
        {
            try
            {
                for (std::uint32_t i = 0; i < ItersPerThread; ++i)
                {
                    const auto guard = shared.lock(Domain);
                    ++g_counter;
                }
            }
            catch (const std::exception& e)
            {
                std::fprintf(stderr, "thread: %s\n", e.what());
                failures.fetch_add(1);
            }
        });
    return failures.load();
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const char* const stratSuffix = ctx.strategySuffix();

    // 2 = control(0) + one data domain.
    harness::formatSession(2, 4);

    // One session for the whole process; every thread below goes through it.
    auto shared = harness::openSharedSession();
    shared.createDomain(Domain);  // creator participates, so lock() is legal straight away

    const std::uint64_t expected = static_cast<std::uint64_t>(Threads) * ItersPerThread;

    const std::uint32_t cohortFailures = hammer(shared);
    const std::uint64_t cohortCounter = g_counter;
    std::printf("strategy=%s threads=%u iters=%u cohort=default counter=%" PRIu64
                " expected=%" PRIu64 "\n",
                stratSuffix, Threads, ItersPerThread, cohortCounter, expected);
    ctx.check(cohortFailures == 0, "cohorted: every thread ran without exception");
    ctx.check(cohortCounter == expected, "cohorted: no lost update (counter == T*M)");

    shared.setCohortCap(1);  // no batching: each acquire releases and re-acquires ownership
    const std::uint32_t plainFailures = hammer(shared);
    const std::uint64_t plainCounter = g_counter;
    std::printf("strategy=%s threads=%u iters=%u cohort=1 counter=%" PRIu64 " expected=%" PRIu64
                "\n",
                stratSuffix, Threads, ItersPerThread, plainCounter, expected);
    ctx.check(plainFailures == 0, "cap=1: every thread ran without exception");
    ctx.check(plainCounter == expected, "cap=1: no lost update (counter == T*M)");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
