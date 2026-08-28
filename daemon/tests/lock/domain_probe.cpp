// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// domain_probe.cpp -- the operations shared_area.hpp defines, with nothing on top of them.
//
// abi_probe.cpp asserts the layout; everything else reaches these operations through a session or a
// stub daemon. No requester and no daemon here: the area is a file-scope object, and the futex words
// work within one process exactly as they do across two.

#include <pthread.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <string_view>
#include <thread>

#include "cmed/errors.hpp"
#include "cmed/robust_lock.hpp"
#include "common/bitmap.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "harness/helper_area.hpp"
#include "shared/protocol/shared_area.hpp"
#include "shared/util/occupancy.hpp"
#include "tests/probe_context.hpp"

namespace
{

// Value-initialised, so every word starts at zero the way a fresh shm object reads.
cmed::protocol::SharedArea_t g_area{};

constexpr std::uint32_t FirstSlot = 1;

// The longest a field takes: one below MaxName, so the terminator its readers stop at still fits.
constexpr std::string_view FullName = "0123456789abcde";

// The same name one letter shorter, at the higher slot: the scan runs upwards, so a comparison
// stopping at the shorter name's length would answer with the slot below.
constexpr std::string_view ShortName = "0123456789abcd";

// Long enough that a waiter is certainly inside its futex wait, and short enough not to be felt.
constexpr timing::Millis Settle{100};

// What a woken waiter is given to come back in. Far under the deadline any waiter here is built
// with, so a wait that ran out instead of being woken reads as a failure and not as a slow wake.
constexpr timing::Millis ReturnWait{500};

// A waiter that records what it was given, so the parent can ask both whether it came back and what
// it came back with.
class Waiter
{
public:
    template <typename T_Settled>
    Waiter(cmed::protocol::Domain_t& domain, timing::Nanos timeout, T_Settled settled)
        : worker_{[this, &domain, timeout, settled]
                  {
                      started_.store(true, std::memory_order_release);
                      outcome_ = domain.awaitState(timeout, settled);
                      returned_.store(true, std::memory_order_release);
                  }}
    {
        // A case that asserts this thread is still waiting has to know it began first. A fixed sleep
        // from construction would measure thread startup instead, which a loaded machine can outlast.
        const timing::Deadline patience{timing::Secs{5}};
        while (!started_.load(std::memory_order_acquire) && !patience.expired())
        {
            std::this_thread::yield();
        }
    }

    Waiter(const Waiter&) = delete;
    Waiter(Waiter&&) = delete;
    Waiter& operator=(const Waiter&) = delete;
    Waiter& operator=(Waiter&&) = delete;

    ~Waiter()
    {
        join();
    }

    // For a case that reads outcome() while the waiter is still in scope. Idempotent, so the
    // destructor stands for a case that never calls it.
    void join()
    {
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    [[nodiscard]] bool returned() const noexcept
    {
        return returned_.load(std::memory_order_acquire);
    }

    // Read only after the thread has been joined.
    [[nodiscard]] const std::optional<cmed::protocol::RequestState>& outcome() const noexcept
    {
        return outcome_;
    }

private:
    std::atomic<bool> started_{false};
    std::atomic<bool> returned_{false};
    std::optional<cmed::protocol::RequestState> outcome_;
    std::thread worker_;
};

[[nodiscard]] bool awaitReturn(const Waiter& waiter)
{
    return poll::waitUntil([&waiter]
                           {
                               return waiter.returned();
                           },
                           ReturnWait, cmed::harness::ProbePoll);
}

[[nodiscard]] bool isLockHeld(cmed::protocol::RequestState state) noexcept
{
    return state == cmed::protocol::RequestState::LockHeld;
}

// The pair, in the order they are meant to be used. The waiter is parked before the pair runs, so
// what ends its wait is the wake and not the value it would have re-read on its own.
void wakeRequesterReleasesAWaiter(probe::Context& ctx)
{
    ctx.openCase("publish and wakeRequester against awaitState");

    cmed::protocol::Domain_t& domain = cmed::harness::resolveSlot(g_area, FirstSlot);
    domain.publish(cmed::protocol::RequestState::Idle);

    Waiter waiter{domain, timing::Secs{5}, isLockHeld};
    std::this_thread::sleep_for(Settle);
    ctx.check(!waiter.returned(), "a waiter on a state nobody published stays waiting");

    domain.publish(cmed::protocol::RequestState::LockHeld);
    domain.wakeRequester();

    // Well inside the waiter's own deadline: a wake that never arrives ends the wait too, and only
    // the time it took tells the two apart.
    ctx.check(awaitReturn(waiter), "the wake ends that wait long before its deadline");

    waiter.join();
    ctx.check(waiter.outcome() == cmed::protocol::RequestState::LockHeld,
              "and hands back the state that was published");
    ctx.check(domain.getState() == cmed::protocol::RequestState::LockHeld,
              "and publish leaves the state it was given");
}

// The half that says why the store and the bump are one call. A store on its own moves the state and
// leaves the version where it was, so a waiter's sleep starts anyway and it sleeps on work already there.
void aBareStoreWakesNobody(probe::Context& ctx)
{
    ctx.openCase("a state store that skips the seq bump");

    cmed::protocol::Domain_t& domain = cmed::harness::resolveSlot(g_area, FirstSlot);
    domain.publish(cmed::protocol::RequestState::Idle);

    bool sleptThrough = false;
    {
        const Waiter waiter{domain, timing::Secs{5}, isLockHeld};
        std::this_thread::sleep_for(Settle);

        // The state the waiter is waiting for, moved without the bump and without the wake.
        cmed::harness::setState(domain, cmed::protocol::RequestState::LockHeld);
        std::this_thread::sleep_for(Settle);
        sleptThrough = !waiter.returned();

        // The same store through publish, which is the whole of the difference.
        domain.publish(cmed::protocol::RequestState::LockHeld);
        domain.wakeRequester();
    }

    ctx.check(sleptThrough, "a bare store leaves the waiter asleep on a state it wanted");
}

void awaitStateGivesUpAtItsDeadline(probe::Context& ctx)
{
    ctx.openCase("awaitState with nobody publishing");

    cmed::protocol::Domain_t& domain = cmed::harness::resolveSlot(g_area, FirstSlot);
    domain.publish(cmed::protocol::RequestState::Idle);

    constexpr timing::Millis Budget{60};
    const timing::Stopwatch waited;
    const std::optional<cmed::protocol::RequestState> outcome = domain.awaitState(Budget, isLockHeld);

    ctx.check(!outcome.has_value(), "awaitState answers nullopt");
    ctx.check(waited.elapsed() >= Budget, "and waited out the deadline it was given");
}

// The value that comes back is the state that satisfied the predicate. Two different settled states
// through one predicate, because a version answering a fixed value would pass with only one.
void awaitStateAnswersWithTheSettledState(probe::Context& ctx)
{
    ctx.openCase("what awaitState comes back with");

    cmed::protocol::Domain_t& domain = cmed::harness::resolveSlot(g_area, FirstSlot);
    const auto isAnswered = [](cmed::protocol::RequestState state)
    {
        return cmed::protocol::isAcquireAnswered(state);
    };

    domain.publish(cmed::protocol::RequestState::LockHeld);
    ctx.check(domain.awaitState(timing::Secs{1}, isAnswered) == cmed::protocol::RequestState::LockHeld,
              "a settled LockHeld comes back as LockHeld");

    domain.publish(cmed::protocol::RequestState::Error);
    ctx.check(domain.awaitState(timing::Secs{1}, isAnswered) == cmed::protocol::RequestState::Error,
              "and a settled Error comes back as Error");

    // A state the predicate does not accept is not an answer, whatever else is true of it.
    domain.publish(cmed::protocol::RequestState::Granting);
    ctx.check(!domain.awaitState(timing::Millis{20}, isAnswered).has_value(),
              "while an unsettled state runs the wait out instead");

    domain.publish(cmed::protocol::RequestState::Idle);
}

// request is publish and ring together, because either half alone is silent: an untold state leaves the
// requester waiting out its deadline, and a knock with no state behind it wakes the daemon for nothing.
void requestDoesBothHalves(probe::Context& ctx)
{
    ctx.openCase("request");

    constexpr std::uint32_t Slot = 40;
    cmed::protocol::Domain_t& domain = cmed::harness::resolveSlot(g_area, Slot);
    domain.publish(cmed::protocol::RequestState::Idle);

    static_cast<void>(cmed::harness::clearPendingBit(g_area, Slot));
    const std::uint32_t doorbellBefore = cmed::harness::getDoorbell(g_area);
    const std::uint32_t seqBefore = cmed::harness::getSeq(domain);

    g_area.request(Slot, cmed::protocol::RequestState::LockRequested);

    ctx.check(domain.getState() == cmed::protocol::RequestState::LockRequested,
              "the state the requester asked for is published");
    // No wake goes with it, and none is owed: the only thread that sleeps on this word is the one
    // making the request. The moved version is what matters, since it refuses a sleep starting after it.
    ctx.check(cmed::harness::getSeq(domain) != seqBefore,
              "the seq word moved, so a sleep starting after this refuses to begin");
    ctx.check((cmed::harness::getPendingWord(g_area, Slot / bitmap::BitsPerWord) &
               bitmap::makeMask(Slot)) != 0,
              "the domain's pending bit went up");
    ctx.check(cmed::harness::getDoorbell(g_area) == doorbellBefore + 1,
              "and the doorbell knocked exactly once");

    // The other half of the pair: publish alone moves the state and tells the daemon nothing, which
    // reads as a slow daemon rather than a request that was never made.
    static_cast<void>(cmed::harness::clearPendingBit(g_area, Slot));
    const std::uint32_t quietBefore = cmed::harness::getDoorbell(g_area);

    domain.publish(cmed::protocol::RequestState::Granting);

    ctx.check(domain.getState() == cmed::protocol::RequestState::Granting,
              "a bare publish still moves the state");
    ctx.check((cmed::harness::getPendingWord(g_area, Slot / bitmap::BitsPerWord) &
               bitmap::makeMask(Slot)) == 0,
              "but raises no pending bit");
    ctx.check(cmed::harness::getDoorbell(g_area) == quietBefore, "and does not knock");

    static_cast<void>(cmed::harness::clearPendingBit(g_area, Slot));
    domain.publish(cmed::protocol::RequestState::Idle);
}

// Spins rather than sleeping a fixed window, so the case waits exactly as long as it has to and a
// loaded machine costs it time instead of correctness.
[[nodiscard]] bool waitUntilWaiters(const cmed::protocol::Domain_t& domain, std::uint32_t wanted)
{
    const timing::Deadline patience{timing::Secs{5}};
    while (cmed::harness::getWaiters(domain) != wanted)
    {
        if (patience.expired())
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// Robust and process-shared, the way formatArea initialises every mutex in a real area. The file-scope
// area here is zeroed rather than formatted, so a case that takes a mutex has to stand one up itself.
void initRobustMutex(pthread_mutex_t& mutex)
{
    pthread_mutexattr_t attributes;
    static_cast<void>(::pthread_mutexattr_init(&attributes));
    static_cast<void>(::pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED));
    static_cast<void>(::pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST));
    static_cast<void>(::pthread_mutex_init(&mutex, &attributes));
    static_cast<void>(::pthread_mutexattr_destroy(&attributes));
}

// The type the count is kept with, under no mutex at all. One unit per object and a destructor to
// give it back, because a unit never returned reads forever after as demand that is not there.
void aTokenIsOneUnitOfTheCount(probe::Context& ctx)
{
    ctx.openCase("OccupancyToken against a bare count");

    std::atomic<std::uint32_t> queued{0};

    {
        const cmed::util::OccupancyToken outer{queued};
        ctx.check(queued.load(std::memory_order_acquire) == 1, "one token is one unit");

        {
            const cmed::util::OccupancyToken inner{queued};
            ctx.check(queued.load(std::memory_order_acquire) == 2, "and a second object is a second unit");
        }

        ctx.check(queued.load(std::memory_order_acquire) == 1, "the inner scope hands back its own unit only");
    }

    ctx.check(queued.load(std::memory_order_acquire) == 0, "and the count comes back to where it started");

    try
    {
        const cmed::util::OccupancyToken leaving{queued};
        throw cmed::CmedError{"a scope its caller left by throwing"};
    }
    catch (const cmed::CmedError&)
    {
        // @expected: the throw is what this half of the case is about.
    }

    ctx.check(queued.load(std::memory_order_acquire) == 0, "a unit taken in a scope that threw is given back");
}

// waiters is the daemon's input for whether to keep CXL ownership after a release, and the only state
// in which it may be non-zero is a requester blocked on the mutex, not one holding it.
void waitersCountsOnlyTheBlocked(probe::Context& ctx)
{
    ctx.openCase("lockForRequest and the waiters count");

    constexpr std::uint32_t Slot = 45;
    cmed::protocol::Domain_t& domain = cmed::harness::resolveSlot(g_area, Slot);
    initRobustMutex(domain.request.lock);
    cmed::harness::setWaiters(domain, 0);

    {
        const cmed::util::RobustLock uncontended = domain.lockForRequest();
        ctx.check(static_cast<bool>(uncontended), "an uncontended lockForRequest takes the mutex");
        ctx.check(!domain.hasWaiters(), "and leaves the count at zero, because nothing waited");
    }

    std::atomic<bool> secondHolds{false};
    std::atomic<bool> mayRelease{false};
    bool sawOneQueued = false;
    bool zeroWhileHolding = false;

    {
        cmed::util::RobustLock holder = domain.lockForRequest();

        std::thread contender{[&domain, &secondHolds, &mayRelease]
                              {
                                  const cmed::util::RobustLock mine = domain.lockForRequest();
                                  secondHolds.store(true, std::memory_order_release);
                                  while (!mayRelease.load(std::memory_order_acquire))
                                  {
                                      std::this_thread::yield();
                                  }
                              }};

        sawOneQueued = waitUntilWaiters(domain, 1);
        holder.unlock();

        const timing::Deadline patience{timing::Secs{5}};
        while (!secondHolds.load(std::memory_order_acquire) && !patience.expired())
        {
            std::this_thread::yield();
        }
        zeroWhileHolding = !domain.hasWaiters();

        mayRelease.store(true, std::memory_order_release);
        contender.join();
    }

    ctx.check(sawOneQueued, "a requester blocked on the mutex reads as one waiter");
    ctx.check(secondHolds.load(std::memory_order_acquire), "the blocked requester goes on to hold it");
    ctx.check(zeroWhileHolding, "and the count is back to zero once it holds, not while it holds");
    ctx.check(!domain.hasWaiters(), "with nothing left behind at the end");
}

// The path the bundling exists for. A mutex left inconsistent turns NOTRECOVERABLE, so the acquire
// inside lockForRequest throws, and the count has to come back down on the way out.
void aThrowInsideTheAcquireLeavesNoCount(probe::Context& ctx)
{
    ctx.openCase("lockForRequest when the acquire throws");

    constexpr std::uint32_t Slot = 46;
    cmed::protocol::Domain_t& domain = cmed::harness::resolveSlot(g_area, Slot);
    initRobustMutex(domain.request.lock);
    cmed::harness::setWaiters(domain, 0);

    // A thread that dies holding a robust mutex hands the next locker EOWNERDEAD, exactly as a dead
    // process would. Unlocking without pthread_mutex_consistent is what makes it NOTRECOVERABLE.
    std::thread abandoner{[&domain]
                          {
                              static_cast<void>(::pthread_mutex_lock(&domain.request.lock));
                          }};
    abandoner.join();

    const int inherited = ::pthread_mutex_lock(&domain.request.lock);
    if (!ctx.check(inherited == EOWNERDEAD, "a mutex its holder never released answers EOWNERDEAD"))
    {
        return;
    }
    static_cast<void>(::pthread_mutex_unlock(&domain.request.lock));

    bool threw = false;
    try
    {
        static_cast<void>(domain.lockForRequest());
    }
    catch (const cmed::CmedError&)
    {
        threw = true;
    }

    if (!ctx.check(threw, "lockForRequest throws once the mutex is past recovering"))
    {
        return;
    }
    ctx.check(!domain.hasWaiters(), "and the queued count came back down on the way out");
}

// The rule the header states and nothing checked: live is what makes a slot a domain, not the bytes
// in its name field.
void aFreedSlotMatchesNothing(probe::Context& ctx)
{
    ctx.openCase("a slot whose live word went back to zero");

    constexpr std::uint32_t Slot = 20;
    constexpr std::string_view Name = "lane-freed";

    cmed::harness::publishDomain(g_area, Slot, Name);
    ctx.check(g_area.resolve(Name) == Slot, "a live slot resolves by name");
    ctx.check(cmed::harness::resolveSlot(g_area, Slot).isNamed(Name), "and says so itself");

    cmed::harness::resolveSlot(g_area, Slot).markState(cmed::protocol::DomainState::Free);

    ctx.check(cmed::harness::resolveSlot(g_area, Slot).getName() == Name,
              "the name bytes are still in the field");
    ctx.check(!cmed::harness::resolveSlot(g_area, Slot).isNamed(Name), "but a freed slot is named nothing");
    ctx.check(g_area.resolve(Name) == cmed::protocol::NoDomain, "and the lookup no longer finds it");
}

void aNameThatFillsTheFieldResolves(probe::Context& ctx)
{
    ctx.openCase("a name with no terminator");

    constexpr std::uint32_t FullSlot = 30;
    constexpr std::uint32_t ShortSlot = 31;

    cmed::harness::publishDomain(g_area, FullSlot, FullName);
    cmed::harness::publishDomain(g_area, ShortSlot, ShortName);

    ctx.check(g_area.resolve(FullName) == FullSlot, "the longest storable name resolves to its own slot");
    ctx.check(g_area.resolve(ShortName) == ShortSlot, "and one letter shorter resolves to the slot above");
    ctx.check(!cmed::harness::resolveSlot(g_area, FullSlot).isNamed(ShortName), "the full field is not named by its own prefix");
    ctx.check(!cmed::harness::resolveSlot(g_area, ShortSlot).isNamed(FullName), "nor the shorter one by the longer");

    // Filling the field, which no write takes, so it can be no stored name at all.
    ctx.check(g_area.resolve("0123456789abcdef") == cmed::protocol::NoDomain, "and a name past the field matches none");
}

// A caller reading 0 as "not found" would take domain 0 for an absent name, which is the reason
// NoDomain is the maximum rather than zero.
void anAbsentNameAnswersNoDomain(probe::Context& ctx)
{
    ctx.openCase("an absent name against domain zero");

    constexpr std::string_view ZeroName = "lane-zero";
    cmed::harness::publishDomain(g_area, 0, ZeroName);

    ctx.check(g_area.resolve(ZeroName) == 0, "a domain at slot zero resolves to zero");
    ctx.check(g_area.resolve("lane-absent") == cmed::protocol::NoDomain, "an absent name answers NoDomain");
    ctx.check(cmed::protocol::NoDomain != 0, "and NoDomain is not slot zero");
}

// The early return is invisible from outside unless the doorbell is read: a knock with no bit up
// would wake a daemon to drain an empty bitmap.
void ringRefusesAnIdPastTheTable(probe::Context& ctx)
{
    ctx.openCase("ring past the end of the table");

    cmed::harness::clearEveryPendingBit(g_area);
    const std::uint32_t before = cmed::harness::getDoorbell(g_area);

    g_area.ring(cmed::MaxDomains);
    g_area.ring(cmed::protocol::NoDomain);

    ctx.check(cmed::harness::getEveryPendingBit(g_area) == 0, "an id past the table raises no bit");
    ctx.check(cmed::harness::getDoorbell(g_area) == before, "and does not knock");

    g_area.ring(FirstSlot);
    ctx.check(cmed::harness::getPendingWord(g_area, 0) ==
                  bitmap::makeMask(FirstSlot),
              "while an id inside it raises its own bit");
    ctx.check(cmed::harness::getDoorbell(g_area) == before + 1, "and knocks exactly once");
}

}  // namespace

int main()
{
    return probe::run("domain probe",
                      [](probe::Context& ctx)
                      {
                          wakeRequesterReleasesAWaiter(ctx);
                          aBareStoreWakesNobody(ctx);
                          awaitStateGivesUpAtItsDeadline(ctx);
                          awaitStateAnswersWithTheSettledState(ctx);
                          requestDoesBothHalves(ctx);
                          aTokenIsOneUnitOfTheCount(ctx);
                          waitersCountsOnlyTheBlocked(ctx);
                          aThrowInsideTheAcquireLeavesNoCount(ctx);
                          aFreedSlotMatchesNothing(ctx);
                          aNameThatFillsTheFieldResolves(ctx);
                          anAbsentNameAnswersNoDomain(ctx);
                          ringRefusesAnIdPastTheTable(ctx);
                      });
}
