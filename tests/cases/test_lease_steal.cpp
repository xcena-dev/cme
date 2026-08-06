// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_lease_steal.cpp -- the admission lease when its holder never gives it back.
//
// Every join takes the membership lease, and every case in the tree takes it from a peer that then
// releases it. So the lease's whole failure side had never run: the stall window, the steal that
// follows it, and the deadline that gives up. Those three are what keep a peer that died between
// staking the lease and releasing it from closing the region to every later joiner.
//
// The lease is a nonce in one 64 B line, not a lock the kernel knows about, so a test can write it
// by hand. That is what both cases here do, and it is also what a crashed peer leaves behind: a
// nonzero nonce with nobody to clear it. Nothing distinguishes the two, which is the point.
//
// A stalled lease is a nonce that does not change for LeaseTimeout. A contended one is a nonce that
// keeps changing, which is why the second case runs a thread doing nothing but changing it. The
// joiner cannot tell that thread from a run of real joiners, and it must not steal a lease that is
// still moving -- so it recontends until LeaseAcquireDeadline and then reports failure.
//
// shm only. Both cases turn on wall-clock windows measured against LeaseTimeout, and the devdax
// and uc variants would spend the same seconds re-asserting a timing rule that reads no medium
// state beyond that one line.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ratio>
#include <thread>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "config.hpp"
#include "core/layout/geometry.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

constexpr std::uint32_t FormatDomains = 2;
constexpr std::uint32_t FormatPeers = 4;

// A nonce no getRandomNonce would produce twice, so the assertions below can name it. Any nonzero
// value reads as held; zero is the unlocked sentinel.
constexpr std::uint64_t StaleNonce = 0xDEADBEEFCAFEF00DULL;

// How often the contending thread restamps the nonce. Shorter than ClaimSettle on purpose: the
// joiner stakes its own nonce and re-reads it one settle later, so a restamp inside that window is
// what makes the stake fail and the joiner recontend.
constexpr auto ChurnPeriod = std::chrono::milliseconds{1};

void storeNonce(cme::Geometry::AdmissionControl_t* control, std::uint64_t nonce,
                cme::CoherencyMode coherency)
{
    auto line = cme::coherency::get(control, coherency);
    line.nonce = nonce;
    cme::coherency::set(control, line, coherency);
}

[[nodiscard]] std::uint64_t loadNonce(cme::Geometry::AdmissionControl_t* control,
                                      cme::CoherencyMode coherency)
{
    return cme::coherency::get(control, coherency).nonce;
}

// ── a lease its holder abandoned ────────────────────────────────────
void checkStalledLeaseIsStolen(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();
    harness::formatSession(ctx, FormatDomains, FormatPeers);

    auto view = harness::openBoundRegion(ctx, std::chrono::milliseconds{1000});
    storeNonce(view.getAdmissionControl(), StaleNonce, coherency);

    const auto began = std::chrono::steady_clock::now();
    {
        auto session = harness::openSession(ctx);
        session.createDomain("lane0");
        const auto guard = session.lock("lane0");
        ctx.check(static_cast<bool>(guard), "stalled lease: the joiner got in and can lock");
    }
    // The join had to watch the nonce for a full LeaseTimeout before it was allowed to steal, so
    // anything faster means it took the lease while the holder still looked alive.
    const double elapsedMs = harness::msSince(began);
    const double stallMs = std::chrono::duration<double, std::milli>(cme::LeaseTimeout).count();
    ctx.checkf(elapsedMs >= stallMs, "stalled lease: the joiner waited out the stall (%.0f ms)",
               elapsedMs);

    // The stolen lease was released on the way out, so the line is free for the next joiner rather
    // than carrying either nonce.
    ctx.check(loadNonce(view.getAdmissionControl(), coherency) == 0,
              "stalled lease: the joiner released what it stole");
}

// ── a lease that keeps moving ───────────────────────────────────────
void checkContendedLeaseGivesUp(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();
    harness::formatSession(ctx, FormatDomains, FormatPeers);

    auto view = harness::openBoundRegion(ctx, std::chrono::milliseconds{1000});
    auto* control = view.getAdmissionControl();

    std::atomic<bool> churning{true};
    std::atomic<std::uint64_t> stamps{0};
    std::thread contender{[control, coherency, &churning, &stamps]
                          {
                              while (churning.load(std::memory_order_relaxed))
                              {
                                  // Never zero: a free lease is staked rather than watched, and the
                                  // path under test is the one where the lease looks held.
                                  storeNonce(control, StaleNonce + stamps.load() + 1, coherency);
                                  stamps.fetch_add(1, std::memory_order_relaxed);
                                  std::this_thread::sleep_for(ChurnPeriod);
                              }
                          }};

    // The joiner sees the nonce change on nearly every watch, so it never reaches the steal and
    // spends its whole deadline recontending.
    const auto joinRegion = [&ctx]
    {
        static_cast<void>(harness::openSession(ctx));
    };
    ctx.check(harness::threw<cme::NoFreeSlotError>(joinRegion),
              "contended lease: the joiner gives up at the deadline");

    churning.store(false, std::memory_order_relaxed);
    contender.join();

    // Evidence the contention was real rather than one stamp the joiner happened to miss: the
    // thread restamped the line throughout the deadline it spent.
    const std::uint64_t expected =
        static_cast<std::uint64_t>(cme::LeaseAcquireDeadline / ChurnPeriod) / 2;
    ctx.checkf(stamps.load() >= expected,
               "contended lease: the nonce kept moving (%llu stamps)",
               static_cast<unsigned long long>(stamps.load()));

    storeNonce(control, 0, coherency);  // leave the region joinable for whatever runs next
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    checkStalledLeaseIsStolen(ctx);
    checkContendedLeaseGivesUp(ctx);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
