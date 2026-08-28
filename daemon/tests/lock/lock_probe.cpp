// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// lock_probe.cpp -- the requester half of the state machine, against a stub daemon.
//
// The stub does only what the real daemon will do to the words: it drains the pending bitmap and
// walks each domain from LockRequested to LockHeld. A release passes through nothing of the stub's,
// since a holder publishes Idle itself and only rings. No cme::Session, no CXL ownership: what is
// under test is the requester's side of the exchange and the domain mutex that keeps two apart.

#include <sys/types.h>

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <thread>
#include <vector>

#include "cmed/errors.hpp"
#include "cmed/guard.hpp"
#include "cmed/session.hpp"
#include "common/bitmap.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "daemon/startup/served_area.hpp"
#include "harness/helper_area.hpp"
#include "harness/helper_requester.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/area.hpp"
#include "shared/protocol/shared_area.hpp"
#include "shared/util/futex.hpp"

namespace
{

// This uid alone. No case here is about who else may open the area.
constexpr mode_t AreaMode = 0600;

constexpr const char* AreaName = "lock-probe";
constexpr std::string_view DomainName = "lane0";

// Slot 0 is the control domain, so a data domain starts at 1.
constexpr std::uint32_t DomainSlot = 1;

// Serves the area the way the daemon will: read the doorbell before draining, so a request that
// arrives mid-drain moves the word and the next wait returns at once instead of sleeping past it.
class StubDaemon
{
public:
    explicit StubDaemon(cmed::protocol::SharedArea_t& area)
        : area_{&area},
          worker_{[this]
                  {
                      run();
                  }}
    {
    }

    StubDaemon(const StubDaemon&) = delete;
    StubDaemon(StubDaemon&&) = delete;
    StubDaemon& operator=(const StubDaemon&) = delete;
    StubDaemon& operator=(StubDaemon&&) = delete;

    ~StubDaemon()
    {
        running_.store(false, std::memory_order_release);
        cmed::util::wakeAllWaiters(cmed::harness::resolveDoorbellWord(*area_));
        worker_.join();
    }

    [[nodiscard]] std::uint32_t grants() const noexcept
    {
        return grants_.load(std::memory_order_acquire);
    }

    // Same lag as harness::StubDaemon: the wake reaches the requester before this count moves.
    [[nodiscard]] bool awaitGrants(std::uint32_t wanted, timing::Millis within = timing::Millis{2000}) const
    {
        return poll::waitUntil([this, wanted]
                               {
                                   return grants_.load(std::memory_order_acquire) >= wanted;
                               },
                               within, cmed::harness::ProbePoll);
    }

private:
    void run()
    {
        while (running_.load(std::memory_order_acquire))
        {
            const std::uint32_t seen = cmed::harness::getDoorbell(*area_);
            if (!drain())
            {
                static_cast<void>(cmed::util::waitOnWord(cmed::harness::resolveDoorbellWord(*area_), seen, timing::Millis{5}));
            }
        }
    }

    // Takes each word whole before handling any of it, so a bit raised mid-pass lands on a word
    // this pass has already cleared and the next pass sees it.
    [[nodiscard]] bool drain()
    {
        bool served = false;
        bitmap::Bits<cmed::MaxDomains> taken = cmed::harness::takeEveryPendingBit(*area_);
        while (!taken.isEmpty())
        {
            serve(taken.popLowest());
            served = true;
        }
        return served;
    }

    void serve(std::uint32_t domainId)
    {
        cmed::protocol::Domain_t& domain = cmed::harness::resolveSlot(*area_, domainId);
        switch (domain.getState())
        {
            case cmed::protocol::RequestState::LockRequested:
                domain.publish(cmed::protocol::RequestState::Granting);
                domain.wakeRequester();
                domain.publishGrant();
                domain.wakeRequester();
                grants_.fetch_add(1, std::memory_order_release);
                break;
            default:
                // Idle is a holder that left with nothing queued behind it, and there is no turn here to
                // hand back. The rest are answers already delivered.
                break;
        }
    }

    cmed::protocol::SharedArea_t* area_;
    std::atomic<bool> running_{true};
    std::atomic<std::uint32_t> grants_{0};
    std::thread worker_;
};

bool oneLockWalksTheMachine(cmed::CmedArea& held, const cmed::harness::StubSetup& setup,
                            const StubDaemon& daemon)
{
    cmed::protocol::SharedArea_t& area = held.shared();
    cmed::CmedSession session = setup.openRequester();
    {
        const cmed::CmedGuard guard = session.lock(DomainName);
        if (!guard || !cmed::harness::isHeld(area, DomainSlot))
        {
            return false;
        }
    }

    // Idle is the holder's own store, so it is already there when the guard is gone. Only grants()
    // itself needs the wait below: the wake reaches the requester before this stub's count moves.
    static_cast<void>(daemon.awaitGrants(1));
    return daemon.grants() == 1 && cmed::harness::isIdle(area, DomainSlot);
}

// Held long enough to see a violation. A domain taken and dropped in one instruction would pass
// whether or not anything excluded the second thread.
bool excludesEveryOtherHolder(const cmed::harness::StubSetup& setup)
{
    constexpr std::uint32_t Threads = 4;
    constexpr std::uint32_t Rounds = 20;

    std::atomic<std::uint32_t> inside{0};
    std::atomic<bool> overlapped{false};
    std::uint32_t counter = 0;  // plain on purpose: a second holder corrupts it

    std::vector<std::thread> holders;
    holders.reserve(Threads);
    for (std::uint32_t index = 0; index < Threads; ++index)
    {
        holders.emplace_back(
            [&]
            {
                cmed::CmedSession session = setup.openRequester();
                for (std::uint32_t round = 0; round < Rounds; ++round)
                {
                    const cmed::CmedGuard guard = session.lock(DomainName);
                    if (inside.fetch_add(1, std::memory_order_acq_rel) != 0)
                    {
                        overlapped.store(true, std::memory_order_release);
                    }
                    ++counter;
                    std::this_thread::sleep_for(timing::Micros{50});
                    inside.fetch_sub(1, std::memory_order_acq_rel);
                }
            });
    }
    for (std::thread& holder : holders)
    {
        holder.join();
    }

    return !overlapped.load(std::memory_order_acquire) && counter == Threads * Rounds;
}

bool refusesAnUnknownName(const cmed::harness::StubSetup& setup)
{
    cmed::CmedSession session = setup.openRequester();
    try
    {
        static_cast<void>(session.lock("absent"));
    }
    catch (const cmed::CmedUnknownDomainError&)
    {
        return true;
    }
    return false;
}

}  // namespace

int main()
{
    bool passed = false;
    try
    {
        const cmed::harness::ProbeScratch scratch{"lock-probe"};
        cmed::CmedArea area = cmed::daemon::formatArea(AreaName);
        cmed::harness::publishDomain(area.shared(), DomainSlot, DomainName);

        const StubDaemon daemon{area.shared()};
        const cmed::harness::StubSetup setup{scratch.makePath("cmed.sock"), area.descriptor()};
        passed = oneLockWalksTheMachine(area, setup, daemon) && refusesAnUnknownName(setup) &&
                 excludesEveryOtherHolder(setup);

        std::printf("lock probe grants=%" PRIu32 "\n", daemon.grants());
    }
    catch (const cmed::CmedError& failure)
    {
        std::printf("lock probe threw: %s\n", failure.what());
        passed = false;
    }

    return passed ? 0 : 1;
}
