// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_timeout_late_grant.cpp -- a lock that times out strands no domain (report §12).
//
// A grant landing past RequestWithdrawSettle names a peer that has given up, and the orphan sweep
// is what reclaims it. So the property is "the domain comes back either way", not "the settle
// window always catches it". Holder and waiter carry the same window, so the two collide.
// All four strategies: no policy may keep a domain on a peer that returned nothing.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr cme::PeerId MaxPeers = 3;
constexpr cme::PeerId Holder = 0;
constexpr cme::PeerId Waiter = 1;
constexpr cme::PeerId Prober = 2;
constexpr cme::DomainId Ceiling = 2;    // control + one data domain
constexpr cme::DomainId Contested = 1;  // the one every round fights over
constexpr std::uint32_t Rounds = 12;

// The waiter's patience is swept from well under the grip to well over it, so the early rounds
// time out for certain, the late ones acquire for certain, and the middle ones land on the release.
// A fixed window equal to the grip acquires every time: the waiter starts counting only after main
// has seen the holder take it, so its deadline always falls past the release.
constexpr std::uint32_t HoldMs = 60;
constexpr std::uint32_t PatienceStepMs = 2 * HoldMs / Rounds;

// What the prober gets to reclaim a domain a late grant left on the waiter. Wide, because the
// route through the orphan sweep costs poll cycles rather than a handoff.
constexpr std::uint32_t ReclaimDeadlineMs = 5000;

// The holder runs on its own thread: a Peer belongs to the thread that built it, and the waiter's
// tryLock has to overlap the grip rather than follow it.
struct HolderThread_t
{
    std::thread runner;
    std::atomic<bool> stop{false};
    std::atomic<bool> grip{false};     // main raises it to ask for one grip
    std::atomic<bool> holding{false};  // the runner raises it once it has the domain
    std::atomic<std::uint32_t> grips{0};
};

void runHolder(HolderThread_t* holder, cme::Geometry* region)
{
    cme::Peer peer = harness::makePeer(*region, Holder);
    peer.joinDomain(Contested);
    while (!holder->stop.load(std::memory_order_acquire))
    {
        if (!holder->grip.exchange(false, std::memory_order_acq_rel))
        {
            harness::sleepMs(1);
            continue;
        }
        auto guard = peer.tryLock(Contested, std::chrono::seconds{5});
        if (!guard)
        {
            continue;  // could not take it this round; the round below reads holding as false
        }
        holder->holding.store(true, std::memory_order_release);
        harness::sleepMs(HoldMs);
        // Cleared before the release, so a reader that sees it true is certain the guard below
        // is still alive. The other order reports the gap between the two as an overlap.
        holder->holding.store(false, std::memory_order_release);
        guard.reset();  // release timed to land on the waiter's deadline
        holder->grips.fetch_add(1, std::memory_order_relaxed);
    }
}

// How many short attempts the waiter makes while the keeper holds. Any number above one would do:
// the residue this looks for accumulates per attempt, so it only has to accumulate visibly.
constexpr std::uint32_t RefusedAttempts = 8;

// A timed-out acquire leaves no residue on the caller. The ownership pin is where residue would
// sit, and unlock counts a release as the outermost one only at pin count 1: a lock path that
// forgets to drop the pin it took therefore never releases at all, and everyone after it starves.
void checkTimeoutLeavesNoResidue(harness::TestContext& ctx)
{
    auto region = harness::createRegion(Ceiling, MaxPeers);
    harness::seedDataDomains(region, Ceiling - 1);

    std::array<harness::PeerSlot_t, 1> keeper{};
    keeper[0].pinned.store(true);
    harness::spawnPeerWorkers(keeper, 1, region, Ceiling - 1);

    auto waiter = harness::makePeer(region, Waiter);
    waiter.joinDomain(Contested);
    const bool held = harness::waitUntil(
        [&keeper]
        {
            return keeper[0].holding.load(std::memory_order_acquire);
        },
        ReclaimDeadlineMs, 1);

    std::uint32_t refused = 0;
    for (std::uint32_t attempt = 0; held && attempt < RefusedAttempts; ++attempt)
    {
        if (!waiter.tryLock(Contested, std::chrono::milliseconds{5}))
        {
            ++refused;
        }
    }
    harness::joinPeerWorkers(keeper, 1);  // the keeper lets go here

    bool waiterGot = false;
    {
        const auto guard = waiter.tryLock(Contested, std::chrono::milliseconds{ReclaimDeadlineMs});
        waiterGot = guard.has_value();
    }

    auto prober = harness::makePeer(region, Prober);
    prober.joinDomain(Contested);
    const auto reclaimed = prober.tryLock(Contested, std::chrono::milliseconds{ReclaimDeadlineMs});

    ctx.check(held, "the keeper took the domain and kept it");
    ctx.check(refused > 0, "the waiter's short attempts were refused while the keeper held it");
    ctx.check(waiterGot, "the waiter took the domain once the keeper let go");
    ctx.check(reclaimed.has_value(), "and a third peer took it after the waiter released");
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    // First, so its region is gone before the sweep below formats its own.
    checkTimeoutLeavesNoResidue(ctx);

    auto region = harness::createRegion(Ceiling, MaxPeers);
    harness::seedDataDomains(region, Ceiling - 1);

    std::printf("timeout late grant: %u rounds, %u ms grip, patience 5..%u ms (%s, backend=%s)\n",
                Rounds, HoldMs, 5 + (Rounds - 1) * PatienceStepMs, ctx.strategySuffix(),
                ctx.backendName());

    HolderThread_t holder;
    holder.runner = std::thread{runHolder, &holder, &region};

    auto waiter = harness::makePeer(region, Waiter);
    waiter.joinDomain(Contested);
    auto prober = harness::makePeer(region, Prober);
    prober.joinDomain(Contested);

    bool reclaimedEvery = true;
    bool heldNothing = true;
    std::uint32_t timedOutUnderGrip = 0;
    std::uint32_t acquiredUnderGrip = 0;
    for (std::uint32_t round = 0; round < Rounds; ++round)
    {
        // Polled at 1 ms, not waitUntil's 200 ms default: the grip is HoldMs wide, so a step
        // coarser than it misses the window every round and the round below sets up nothing.
        holder.grip.store(true, std::memory_order_release);
        if (!harness::waitUntil(
                [&holder]
                {
                    return holder.holding.load(std::memory_order_acquire);
                },
                ReclaimDeadlineMs, 1))
        {
            continue;  // the holder never got it, so this round sets up nothing
        }

        const auto patience = std::chrono::milliseconds{5 + round * PatienceStepMs};
        auto guard = waiter.tryLock(Contested, patience);
        // Read the grip after the attempt returns, not before it starts. Seeing the round begin
        // inside a grip says nothing about what the attempt met, and a round that met an empty
        // domain judges neither the deadline nor the record.
        const bool gripAlive = holder.holding.load(std::memory_order_acquire);
        if (guard)
        {
            if (gripAlive)
            {
                ++acquiredUnderGrip;  // two guards on one domain
            }
            guard.reset();
        }
        else
        {
            if (gripAlive)
            {
                ++timedOutUnderGrip;
            }
            // The give-up is only honest if the record stopped naming the waiter with it.
            heldNothing =
                heldNothing && harness::readDomainRecord(region, Contested).holder != Waiter;
        }

        // Whatever the waiter got, a third peer must be able to take the domain: by handoff if it
        // is held, by the orphan sweep if a late grant left it on a peer that walked away.
        auto reclaimed = prober.tryLock(Contested, std::chrono::milliseconds{ReclaimDeadlineMs});
        reclaimedEvery = reclaimedEvery && static_cast<bool>(reclaimed);
        reclaimed.reset();
    }

    holder.stop.store(true, std::memory_order_release);
    holder.runner.join();

    std::printf(
        "holder took it %u times; of %u rounds the waiter timed out under a live grip %u times "
        "and got in under one %u times\n",
        holder.grips.load(), Rounds, timedOutUnderGrip, acquiredUnderGrip);

    ctx.check(timedOutUnderGrip > 0, "the window produced at least one timeout inside a grip");
    ctx.check(acquiredUnderGrip == 0, "the waiter took the domain while the holder still held it");
    ctx.check(heldNothing, "a timed-out lock left the record naming somebody else");
    ctx.check(reclaimedEvery, "the domain was acquirable again after every round");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
