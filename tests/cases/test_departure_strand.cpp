// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_departure_strand.cpp -- a grant landing on a departing peer must not strand the domain
// (C4: no RA targets a None slot and no delete path accepts a foreign holder, so the domain
// is lost for good).
//
// ~Peer publishes Leaving and drains with its poll thread still running, so a grant arriving in
// that window has to be forwarded. The LeaveInDrain failpoint holds the departure inside that
// window until the grant is published, so the window is entered rather than aimed at.
//
// --iters sets the iteration count (default DefaultIters); raise it for a long run.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#include "common/args.hpp"
#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "observe/failpoint.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

constexpr int DefaultIters = 200;

// How long the granter waits for the departure to reach the drain. Generous, since what it waits on
// is one thread getting scheduled rather than any work.
constexpr timing::Millis HoldWait{2000};

// The gap between the join and the departure, so the keeper's poll thread has granted at least once
// and the record is live rather than untouched when the departure starts.
constexpr timing::Millis SettleGap{1};

constexpr cme::PeerId Keeper = 0;
constexpr cme::PeerId Leaver = 1;
constexpr cme::PeerId MaxPeers = 2;
constexpr cme::DomainId Ceiling = 2;  // control + 1 data

}  // namespace

void runBody(harness::TestContext& ctx)
{
    using Status = cme::Geometry::Member_t::Status;

    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip(
            "built without CME_FAILPOINT, so the grant cannot be placed inside the drain");
    }

    const int iters = static_cast<int>(cliargs::argU64("--iters", DefaultIters));

    cme::Geometry region = harness::createRegion(Ceiling, MaxPeers);

    std::printf("departure strand: %d forced grants at departure (%s, backend=%s)\n", iters,
                ctx.strategySuffix(), ctx.backendName());

    auto keeper = harness::makePeerPtr(region, Keeper);
    const cme::DomainId domainId = keeper->createDomain("churn").id;
    keeper->joinDomain(domainId);

    auto* recordSlot = region.getDomainRecord(domainId);
    auto* leaverSlot = region.getMemberSlot(Leaver);

    int stranded = 0;
    int forwarded = 0;
    int unsettled = 0;
    int missed = 0;
    for (int iter = 0; iter < iters; ++iter)
    {
        auto leaver = harness::makePeerPtr(region, Leaver);
        leaver->joinDomain(domainId);
        std::this_thread::sleep_for(SettleGap);

        // Armed before the departure starts, so the reach cannot beat the arm.
        cme::failpoint::hold(cme::failpoint::Boundary::LeaveInDrain);

        // Publishes the record naming the leaver while the departure waits inside the drain, which
        // is what a landed grant leaves behind. Releases either way, so a miss costs the hold cap.
        std::atomic<bool> placed{false};
        std::thread granter(
            [&]
            {
                if (cme::failpoint::awaitHeld(HoldWait))
                {
                    auto record = cme::coherency::get(recordSlot, ctx.coherency());
                    record.holder = Leaver;
                    record.epoch = record.epoch + 1;
                    cme::coherency::set(recordSlot, record, ctx.coherency());
                    placed.store(true, std::memory_order_release);
                }
                cme::failpoint::release();
            });

        leaver.reset();  // ~Peer: Leaving -> drain -> stop poll -> hand off -> None
        granter.join();

        // Only judge once the departure is complete: the slot reads None, so the record naming this
        // peer can never be recovered by anything.
        const auto member = cme::coherency::get(leaverSlot, ctx.coherency());
        if (!placed.load(std::memory_order_acquire))
        {
            ++missed;
        }
        else if (!member.hasStatus(Status::None))
        {
            ++unsettled;
        }
        else if (cme::coherency::get(recordSlot, ctx.coherency()).getHolder() == Leaver)
        {
            ++stranded;
            if (stranded <= 5)
            {
                std::printf("  iter %d: domain stranded on the departed peer\n", iter);
            }
        }
        else
        {
            ++forwarded;
        }

        // Put the record back on the keeper so the next iteration starts clean.
        {
            auto reset = cme::coherency::get(recordSlot, ctx.coherency());
            reset.holder = Keeper;
            reset.epoch = reset.epoch + 1;
            cme::coherency::set(recordSlot, reset, ctx.coherency());
        }
    }

    std::printf("  %d/%d grants forwarded, %d stranded, %d departures unsettled, %d never held\n",
                forwarded, iters, stranded, unsettled, missed);

    ctx.check(missed == 0, "every departure reached the drain and took the grant");
    ctx.check(unsettled == 0, "every departure settled to None");
    ctx.check(stranded == 0, "no forced grant left the domain on the departed peer");

    keeper.reset();  // membership leaves before the mapping it was made against goes
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
