// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_departure_strand.cpp -- a grant landing on a departing peer must not strand the domain
// (C4: no RA targets a None slot and no delete path accepts a foreign holder, so the domain
// is lost for good).
//
// The race is microseconds wide, so this does not wait for it: one thread destroys the peer
// while another publishes the record naming it, which IS what a landed grant leaves behind.
// ~Peer publishes Leaving and drains with its poll thread still running, so a grant arriving
// in that window should be forwarded; one arriving after the poll thread stops cannot be.
// Each iteration spins a different number of times before publishing, sweeping where in the
// departure the grant lands.
//
// --iters sets the iteration count (default DefaultIters); raise it for a long run.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#include "args.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"
#include "util/time.hpp"

namespace test
{
namespace
{

constexpr int DefaultIters = 200;
constexpr cme::PeerId Keeper = 0;
constexpr cme::PeerId Leaver = 1;
constexpr cme::PeerId MaxPeers = 2;
constexpr cme::DomainId Ceiling = 2;  // control + 1 data

}  // namespace

void runBody(harness::TestContext& ctx)
{
    using Status = cme::Geometry::Member_t::Status;

    const int iters = static_cast<int>(harness::argU64("--iters", DefaultIters));

    cme::Geometry region = harness::createRegion(ctx, Ceiling, MaxPeers);

    std::printf("departure strand: %d forced grants at departure (%s, backend=%s)\n", iters,
                ctx.strategySuffix(), ctx.backendName());

    auto keeper = std::make_unique<cme::Peer>(region, Keeper, ctx.coherency());
    const cme::DomainId domainId = keeper->createDomain("churn");
    keeper->joinDomain(domainId);

    auto* recordSlot = region.getDomainRecord(domainId);
    auto* leaverSlot = region.getMemberSlot(Leaver);

    int stranded = 0;
    int landedLate = 0;
    for (int iter = 0; iter < iters; ++iter)
    {
        auto leaver = std::make_unique<cme::Peer>(region, Leaver, ctx.coherency());
        leaver->joinDomain(domainId);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});

        // Publish the grant from another thread, sweeping how deep into the departure it lands.
        std::atomic<bool> barrier{false};
        const std::uint32_t spins = static_cast<std::uint32_t>(iter % 400) * 8;
        std::thread granter(
            [&]
            {
                while (!barrier.load(std::memory_order_acquire))
                {
                }
                for (std::uint32_t i = 0; i < spins; ++i)
                {
                    cme::time::relaxCpu(1);
                }
                auto record = cme::coherency::get(recordSlot, ctx.coherency());
                record.holder = Leaver;
                record.epoch = record.epoch + 1;
                cme::coherency::set(recordSlot, record, ctx.coherency());
            });

        barrier.store(true, std::memory_order_release);
        leaver.reset();  // ~Peer: Leaving -> drain -> stop poll -> hand off -> None
        granter.join();

        // Only judge once the departure is complete: the slot reads None, so the record
        // naming this peer can never be recovered by anything.
        const auto member = cme::coherency::get(leaverSlot, ctx.coherency());
        if (!member.hasStatus(Status::None))
        {
            continue;
        }
        const auto record = cme::coherency::get(recordSlot, ctx.coherency());
        if (record.getHolder() == Leaver)
        {
            ++stranded;
            if (stranded <= 5)
            {
                std::printf("  iter %d (spins=%u): domain stranded on the departed peer\n", iter,
                            spins);
            }
        }
        else
        {
            ++landedLate;
        }

        // Put the record back on the keeper so the next iteration starts clean.
        {
            auto reset = cme::coherency::get(recordSlot, ctx.coherency());
            reset.holder = Keeper;
            reset.epoch = reset.epoch + 1;
            cme::coherency::set(recordSlot, reset, ctx.coherency());
        }
    }

    std::printf("  %d/%d grants forwarded, %d stranded\n", landedLate, iters, stranded);
    ctx.check(stranded == 0, "no forced grant left the domain on the departed peer");

    keeper.reset();  // membership leaves before the mapping it was made against goes
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
