// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_reconcile_stale_belief.cpp -- a reconcile pass that read the truth before its own worker adopted
// the domain must not leave that peer unable to take the domain the truth names it holder of.
//
// The pass is held between its truth read and its belief drop. Its worker takes the lane meanwhile, so the
// drop then lands on a peer the record names, and the case asks whether that peer can lock the lane again.

#include <cstdint>
#include <cstdio>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "observe/failpoint.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr std::uint32_t MaxDomains = 2;  // control + one data
constexpr cme::PeerId MaxPeers = 2;      // the parent holding the lane, and the child taking it
constexpr const char* HeldLane = "lane0";

// The hold lands on the next reconcile pass, one OwnershipCheckInterval away at most.
constexpr std::uint32_t HoldWaitMs = 2000;
constexpr std::uint32_t PeerWaitMs = 4000;
constexpr std::uint32_t ChildWaitMs = 10000;
constexpr std::uint32_t StepMs = 1;
// Past the release, so the held pass has dropped the belief, and well inside the next pass.
constexpr std::uint32_t DropSettleMs = 5;
// Far below the acquire timeout, and ample for a peer the record already names.
constexpr timing::Millis RelockBudget{200};

// One word per signal in a shared scratch buffer, written by one process and read by another.
enum class Signal : std::uint32_t
{
    ChildOpen,     // parent: join the lane
    ChildHeld,     // child: its reconcile pass is held on a read naming the parent
    ChildTook,     // child: its worker took the lane and released it
    ChildRelock,   // child: 1 when the lane locked again within the budget, 2 when it did not
    ChildMissing,  // child: the hold was never reached
    CaseDone,      // parent: the region has been read, exit
    Count,
};

constexpr std::uint32_t RelockOk = 1;
constexpr std::uint32_t RelockRefused = 2;

void raiseSignal(std::uint32_t* words, Signal signal, std::uint32_t value = 1)
{
    __atomic_store_n(&words[static_cast<std::uint32_t>(signal)], value, __ATOMIC_RELEASE);
}

[[nodiscard]] std::uint32_t readSignal(const std::uint32_t* words, Signal signal)
{
    return __atomic_load_n(&words[static_cast<std::uint32_t>(signal)], __ATOMIC_ACQUIRE);
}

[[nodiscard]] bool awaitSignal(const std::uint32_t* words, Signal signal, std::uint32_t deadlineMs)
{
    return harness::waitUntil(
        [words, signal]
        {
            return readSignal(words, signal) != 0;
        },
        deadlineMs, StepMs);
}

// Arms the hold only after its join, which took the control lock, so the one read naming another holder
// is the lane's.
void runChild(std::uint32_t* words)
{
    if (!awaitSignal(words, Signal::ChildOpen, ChildWaitMs))
    {
        return;
    }
    auto session = harness::openSession();
    session.joinDomain(HeldLane);
    cme::failpoint::hold(cme::failpoint::Boundary::ReconcileBeforeLose);
    if (!cme::failpoint::awaitHeld(timing::Millis{HoldWaitMs}))
    {
        cme::failpoint::release();
        raiseSignal(words, Signal::ChildMissing);
        return;
    }
    raiseSignal(words, Signal::ChildHeld);

    {
        const auto guard = session.lock(HeldLane);
    }
    raiseSignal(words, Signal::ChildTook);

    cme::failpoint::release();
    harness::sleepMs(DropSettleMs);
    const bool relocked = harness::canLock(session, HeldLane, RelockBudget);
    raiseSignal(words, Signal::ChildRelock, relocked ? RelockOk : RelockRefused);
    static_cast<void>(awaitSignal(words, Signal::CaseDone, ChildWaitMs));
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so the reconcile pass cannot be held");
    }

    harness::formatSession(MaxDomains, MaxPeers);
    const auto signals = ctx.scratch<std::uint32_t>("signals", static_cast<std::uint32_t>(Signal::Count));
    if (!ctx.check(static_cast<bool>(signals), "shared signal buffer mapped"))
    {
        return;
    }
    std::uint32_t* words = signals.data();

    // Forked before the parent opens, so the child copies no poll thread and inherits no hold.
    const std::uint32_t spawned = harness::spawnChildren(1,
                                                         [words](std::uint32_t)
                                                         {
                                                             runChild(words);
                                                         });

    // The creator holds the lane and stays idle, so the child's demand is granted from the poll thread.
    auto holder = harness::openSession();
    holder.createDomain(HeldLane);

    raiseSignal(words, Signal::ChildOpen);
    const bool childHeld = awaitSignal(words, Signal::ChildHeld, PeerWaitMs);
    const bool childTook = childHeld && awaitSignal(words, Signal::ChildTook, PeerWaitMs);
    const bool relockReported = childTook && awaitSignal(words, Signal::ChildRelock, PeerWaitMs);
    const std::uint32_t relock = readSignal(words, Signal::ChildRelock);
    std::printf("  child relock after the held drop: %s\n",
                relock == RelockOk ? "locked" : (relock == RelockRefused ? "refused" : "not reported"));

    raiseSignal(words, Signal::CaseDone);
    harness::reapChildren(spawned);

    ctx.check(childHeld, "the child's reconcile pass stopped on a read naming the parent");
    ctx.check(childTook, "the child's worker took the lane while that pass was held");
    ctx.check(relockReported && relock == RelockOk, "the child locked the lane it holds again after the held drop");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
