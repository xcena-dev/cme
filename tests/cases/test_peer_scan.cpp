// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_peer_scan.cpp -- the peer bound on its own admission line keeps every member in scope.
//
// A joiner sets the bound past the highest slot in use under the admission lease. The case checks what
// scans need from that: a joiner is granted a domain another session holds, joiners arriving together are
// all granted, a frozen peer below the bound is found and recovered, and a later join lowers the bound to
// the slots still in use. It also prints how long a new session waits for its first grant.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <thread>
#include <vector>

#include "cme/shared.hpp"
#include "common/ordered.hpp"
#include "common/timing.hpp"
#include "config.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

// Slot 0 is control, slot 1 the domain every joiner asks for.
constexpr std::uint32_t Domains = 2;
constexpr std::uint32_t Peers = 4;
constexpr const char* LaneName = "lane";

constexpr cme::PeerId FirstSlot = 0;
constexpr cme::PeerId JoinerSlot = 1;

// Joins timed one after another, each a different fraction of a scan period after the last leave, so
// the samples span the wait instead of repeating one point of it.
constexpr std::uint32_t JoinSamples = 5;
constexpr auto PhaseStep = cme::config::poll_thread::ScanRefreshInterval / JoinSamples;

// Every slot but the first session's joins at once: the shape a burst of process starts takes.
constexpr std::uint32_t BurstJoiners = Peers - 1;
constexpr std::uint32_t BurstHoldMs = 5000;

constexpr timing::Millis GrantBudget{5000};
// The burst's last slot: a Peer placed by id grows no bound, so the frozen peer is put below the one the
// burst left.
constexpr cme::PeerId FrozenSlot = Peers - 1;
// A frozen peer goes None after the dead window and the recovery span.
constexpr std::uint32_t RecoveryWaitMs = 15000;

[[nodiscard]] std::uint32_t readScanBound(const cme::Geometry& region)
{
    return cme::coherency::get(region.getPeerBound(), harness::currentRun().coherency()).peerScanBound;
}

// One session joining the lane and asking for it; true when the grant came. A join that times out is a
// false here rather than a throw, so the timed loop reports it as a failed check.
[[nodiscard]] bool joinAndTake()
{
    try
    {
        auto joiner = harness::openSession();
        joiner.joinDomain(LaneName);
        return harness::canLock(joiner, LaneName, GrantBudget);
    }
    catch (const std::exception& failure)
    {
        harness::log("join failed: %s", failure.what());
        return false;
    }
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    harness::formatSession(Domains, Peers);
    cme::Geometry region = harness::openBoundRegion();
    auto first = harness::openSession();
    first.createDomain(LaneName);
    ctx.check(readScanBound(region) == FirstSlot + 1, "the first session's join bounds scans at its slot");

    bool everyJoinerGranted = true;
    std::vector<std::uint64_t> grantMicros;
    for (std::uint32_t sample = 0; sample < JoinSamples; ++sample)
    {
        std::this_thread::sleep_for(PhaseStep * sample);
        const timing::Stopwatch sinceOpen;
        everyJoinerGranted = joinAndTake() && everyJoinerGranted;
        grantMicros.push_back(static_cast<std::uint64_t>(sinceOpen.elapsed<timing::Micros>().count()));
    }
    ctx.check(everyJoinerGranted, "every joiner took the domain the first session created");

    std::sort(grantMicros.begin(), grantMicros.end());
    constexpr double MicrosPerMilli = 1000.0;
    std::printf("open to first grant over %u joins: min %.1f ms, median %.1f ms, max %.1f ms\n", JoinSamples,
                harness::percentile(grantMicros, 0.0) / MicrosPerMilli,
                harness::percentile(grantMicros, 0.5) / MicrosPerMilli,
                harness::percentile(grantMicros, 1.0) / MicrosPerMilli);

    // Each joiner stays until all are granted, so the burst holds every slot at once and sets the bound over all.
    ordered::Published<std::uint32_t> burstGranted{0};
    const auto isBurstGranted = [&]
    {
        return burstGranted.load() == BurstJoiners;
    };
    harness::runThreads(BurstJoiners,
                        [&](std::uint32_t /*index*/)
                        {
                            try
                            {
                                auto joiner = harness::openSession();
                                joiner.joinDomain(LaneName);
                                if (harness::canLock(joiner, LaneName, GrantBudget))
                                {
                                    burstGranted.add(1);
                                }
                                static_cast<void>(harness::waitUntil(isBurstGranted, BurstHoldMs));
                            }
                            catch (const std::exception& failure)
                            {
                                harness::log("burst join failed: %s", failure.what());
                            }
                        });
    ctx.check(burstGranted.load() == BurstJoiners,
              "joiners arriving together were all granted the domain");

    // Leaked, as a crash leaves it: no destructor touches the slot recovery frees. Frozen, its poll
    // thread reads only its own memory, so the region may go before it does.
    auto* frozen = harness::makePeerPtr(region, FrozenSlot).release();
    frozen->setFreeze(true);
    const auto isFrozenSlotFree = [&]
    {
        return harness::hasMemberStatus(region, FrozenSlot, Status::None);
    };
    ctx.check(harness::waitUntil(isFrozenSlotFree, RecoveryWaitMs),
              "a frozen peer below the bound is found and recovered");

    // Every slot but the first is free now; the next join takes the lowest and lowers the bound to it.
    auto stayer = harness::openSession();
    ctx.check(readScanBound(region) == JoinerSlot + 1, "a join lowers the bound past departures and a recovery");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
