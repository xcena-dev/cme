// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// sync_probe.cpp -- the two primitives a requester holds a domain with.
//
// RobustLock's whole reason to exist is the paths where a plain lock/unlock pair does not run: a
// throw between them, a move that hands ownership on, a holder that dies. Each of those gets a case
// here, because none of them shows up in a run where nothing goes wrong.

#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <thread>
#include <utility>

#include "cmed/errors.hpp"
#include "cmed/robust_lock.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "daemon/startup/served_area.hpp"
#include "harness/helper_area.hpp"
#include "shared/area.hpp"
#include "shared/protocol/shared_area.hpp"
#include "shared/util/futex.hpp"

namespace
{

// This uid alone. No case here is about who else may open the area.
constexpr mode_t AreaMode = 0600;

constexpr const char* AreaName = "sync-probe";

// No name is published on it, so nothing resolves to it and its mutex and its seq word are this
// probe's alone.
constexpr std::uint32_t DomainSlot = 2;

// Gap between looks while the wake case watches its waiter.
constexpr timing::Millis PollStep{2};

// A thread reaching its first statement, which a loaded machine can delay.
constexpr timing::Millis StartWait{5000};

// Long enough that the waiter is past its spin and inside the futex wait.
constexpr timing::Millis Settle{100};

// What a woken waiter is given to come back in, far under the deadline it waits with.
constexpr timing::Millis ReturnWait{500};

// Asked from another thread, because a robust mutex is not recursive: the owner asking about its
// own mutex is a different question from the one this answers.
bool isFree(pthread_mutex_t& mutex)
{
    bool taken = false;
    std::thread asker{[&mutex, &taken]
                      {
                          const std::optional<cmed::util::RobustLock> held = cmed::util::RobustLock::tryLock(mutex);
                          taken = held.has_value();
                      }};
    asker.join();
    return taken;
}

bool releasesOnScopeExit(pthread_mutex_t& mutex)
{
    {
        const cmed::util::RobustLock held{mutex};
        if (!held || isFree(mutex))
        {
            return false;
        }
    }
    return isFree(mutex);
}

// The release has to survive the stack unwinding, since a requester's span covers futex waits that
// can throw.
bool releasesOnThrow(pthread_mutex_t& mutex)
{
    try
    {
        const cmed::util::RobustLock held{mutex};
        throw cmed::CmedError{"sync probe: deliberate"};
    }
    catch (const cmed::CmedError&)
    {
        return isFree(mutex);
    }
}

// The moved-from lock must not unlock at its own scope exit, or the span the destination is inside
// would end early and a second requester would enter it.
bool moveTransfersOwnership(pthread_mutex_t& mutex)
{
    cmed::util::RobustLock destination;
    {
        cmed::util::RobustLock source{mutex};
        destination = std::move(source);
        // NOLINTNEXTLINE(bugprone-use-after-move): reading the moved-from lock is the assertion.
        if (source)
        {
            return false;
        }
    }

    if (isFree(mutex) || !destination)
    {
        return false;
    }

    destination.unlock();
    return isFree(mutex) && !destination;
}

bool refusesWhileAnotherThreadHolds(pthread_mutex_t& mutex)
{
    const cmed::util::RobustLock held{mutex};
    return static_cast<bool>(held) && !isFree(mutex);
}

// The child dies inside the span. The parent must be told the guarded state may be half written,
// and the mutex must still be usable afterwards, which is what proves consistent() ran.
bool recoversFromADeadHolder(pthread_mutex_t& mutex)
{
    const auto child = ::fork();
    if (child < 0)
    {
        return false;
    }
    if (child == 0)
    {
        // _exit, not return: an unwound stack would run this copy's destructors and release the
        // very lock the parent has to find abandoned.
        ::_exit(::pthread_mutex_lock(&mutex) == 0 ? 0 : 1);
    }

    int status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        return false;
    }

    {
        const cmed::util::RobustLock inherited{mutex};
        if (!inherited || !inherited.wasAbandoned())
        {
            return false;
        }
    }

    // tryLock, so a mutex the block above failed to release answers EBUSY instead of holding
    // this probe against itself. A mutex left inconsistent throws ENOTRECOVERABLE here.
    const std::optional<cmed::util::RobustLock> afterwards = cmed::util::RobustLock::tryLock(mutex);
    return afterwards.has_value() && !afterwards->wasAbandoned();
}

// The waiter sleeps on the value it last read, so the waker has to move that value before waking.
// Storing the same value again would leave the waiter asleep with the work already published.
//
// The bump and the wake go out once, after the waiter is parked. A wake sent before it reaches the
// syscall is not queued for it, so a loop of bumps would end the wait through the value it re-reads
// on its own and would say nothing about the wake.
bool wakesAWaiterOnTheSeqWord(std::atomic<std::uint32_t>& word)
{
    const std::uint32_t observed = word.load(std::memory_order_acquire);
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    bool woke = false;

    std::thread waiter{[&word, observed, &entered, &returned, &woke]
                       {
                           entered.store(true, std::memory_order_release);
                           woke = cmed::util::waitOnWord(word, observed, timing::Secs{5});
                           returned.store(true, std::memory_order_release);
                       }};

    const bool started = poll::waitUntil([&entered]
                                         {
                                             return entered.load(std::memory_order_acquire);
                                         },
                                         StartWait, PollStep);
    std::this_thread::sleep_for(Settle);

    // Nothing has moved the word, so a waiter that is already back was never asleep on it and no
    // wake below can be credited with ending the wait.
    const bool stayed = !returned.load(std::memory_order_acquire);

    word.fetch_add(1, std::memory_order_release);
    cmed::util::wakeAllWaiters(word);

    // Well inside the waiter's own deadline: a wait that runs out ends too, and the time it took is
    // the only thing that tells the deadline from the wake.
    const bool cameBack = poll::waitUntil([&returned]
                                          {
                                              return returned.load(std::memory_order_acquire);
                                          },
                                          ReturnWait, PollStep);

    waiter.join();
    return started && stayed && cameBack && woke;
}

// A word nobody touches must end the wait on the deadline rather than hold the caller forever.
bool returnsOnItsDeadline(std::atomic<std::uint32_t>& word)
{
    const std::uint32_t observed = word.load(std::memory_order_acquire);
    const timing::Stopwatch waited;
    const bool woke = cmed::util::waitOnWord(word, observed, timing::Millis{20});

    return !woke && waited.elapsed() >= timing::Millis{20};
}

}  // namespace

int main()
{
    bool passed = false;
    try
    {
        cmed::CmedArea area = cmed::daemon::formatArea(AreaName);
        cmed::protocol::Domain_t& context = cmed::harness::resolveSlot(area.shared(), DomainSlot);
        std::atomic<std::uint32_t>& word = cmed::harness::resolveSeqWord(area.shared(), DomainSlot);

        // The fork case runs first, while this process is still single threaded.
        passed = recoversFromADeadHolder(context.request.lock) &&
                 releasesOnScopeExit(context.request.lock) &&
                 releasesOnThrow(context.request.lock) &&
                 moveTransfersOwnership(context.request.lock) &&
                 refusesWhileAnotherThreadHolds(context.request.lock) &&
                 wakesAWaiterOnTheSeqWord(word) &&
                 returnsOnItsDeadline(word);

        std::printf("sync probe seq=%" PRIu32 "\n", word.load(std::memory_order_acquire));
    }
    catch (const cmed::CmedError& failure)
    {
        std::printf("sync probe threw: %s\n", failure.what());
        passed = false;
    }

    return passed ? 0 : 1;
}
