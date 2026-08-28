// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/protocol/shared_area.hpp -- the area both builds read, and the ABI they agree on to do it.
//
// The layout and the state machine both, since a disagreement about the machine loses requests without
// raising an error. A change to any name here is a wire change: bump AbiVersion.
// Setup and control travel over the socket instead, where the kernel stamps the caller's credentials.
// Both sides compile this with one toolchain, since the area is node-local shm and never reaches CXL.
// Each mutex is PTHREAD_PROCESS_SHARED and PTHREAD_MUTEX_ROBUST, and formatArea owns their init.

#pragma once

#include <pthread.h>  // IWYU pragma: keep (declares pthread_mutex_t through bits/pthreadtypes.h)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "cme/limits.hpp"
#include "cmed/robust_lock.hpp"
#include "common/bitmap.hpp"
#include "common/timing.hpp"
#include "shared/protocol/domain_name.hpp"
#include "shared/util/futex.hpp"
#include "shared/util/occupancy.hpp"

namespace cmed
{

// ── limits ─────────────────────────────────────────────────────────────
// Ceilings the build is compiled against, not tunables: they are the array lengths of
// SharedArea_t, so moving one changes every offset in it. Unscoped so an array bound needs no cast.
//
// Taken from cme rather than copied. A copy is what disagrees with the region the daemon opened
// while both builds still succeed.
enum CmedLimits : std::uint32_t
{
    MaxDomains = cme::MaxDomains,
    MaxName = cme::MaxDomainNameLen,
    CachelineBytes = cme::CacheLineBytes,
};

}  // namespace cmed

// The area's own names, unprefixed because the namespace already says which contract they belong to.
// The limits stay above it: they are cme's, and the config and the mapping owner read them too.
namespace cmed::protocol
{

// ── the area's own constants ───────────────────────────────────────────
// What this contract fixes rather than what a build was given. Unscoped for the same reason as the
// limits above: both are compared against plain words in the area and in a message.
enum AreaContract : std::uint32_t
{
    // What this build lays the area out to. A peer answering with another one is refused before its
    // descriptor is mapped.
    AbiVersion = 1,

    // resolve() answers with it. The maximum and not zero, because zero is a live domain id.
    NoDomain = std::numeric_limits<std::uint32_t>::max(),
};

// ── enumerations ───────────────────────────────────────────────────────

// An acquire only. A release has no state: a holder publishes Idle itself and asks the daemon nothing.
// Narrow on purpose: the word beside it in the contended line records abandonment, and the two
// together fill the four bytes the seq word's alignment asks for anyway.
// What a domain is, as against RequestState below, which is what one request on it is doing. Broken is
// a contract the daemon could not keep, and every reader that asks for Live already refuses it.
enum class DomainState : std::uint32_t
{
    Free = 0,
    Live = 1,
    Broken = 2,
};

enum class RequestState : std::uint16_t
{
    Idle = 0,
    LockRequested = 1,
    Granting = 2,
    LockHeld = 3,
    Error = 4,
};

// Beside the enum, because adding a state means deciding whether it ends a requester's wait.
[[nodiscard]] constexpr bool isAcquireAnswered(RequestState state) noexcept
{
    return state == RequestState::LockHeld || state == RequestState::Error;
}

// ── records ────────────────────────────────────────────────────────────

// One domain, two cachelines. Identity and request state share one object because they share one id: a
// create that wrote the name but not the state would leave a live name over the previous request.
struct alignas(CachelineBytes) Domain_t
{
    // ── the contended line ─────────────────────────────────────────
    // One request's words, and the grouping is the cacheline: split any finer and each subgroup takes
    // its own alignment padding, which pushes the registry line onto a third line for every domain.
    struct
    {
        // Held from before LockRequested until after the critical section. That is what makes two local
        // requesters exclude each other, which the state words alone cannot do.
        pthread_mutex_t lock;

        std::atomic<RequestState> state;

        // Bumped by the acquire that inherits this mutex from a holder that died inside its section.
        // Its own word, so publishing a state neither preserves it nor needs a read-modify-write.
        std::atomic<std::uint16_t> abandonCount;

        // The word every futex wait keys on, bumped after every state store. Waiting on state itself
        // loses a wakeup, since a second store can restore a value the waiter had already read.
        std::atomic<std::uint32_t> seq;

        // Requesters queued on `lock`, not counting the holder. Local demand and not a request count:
        // the daemon reads it to decide whether to keep CXL ownership after a release.
        std::atomic<std::uint32_t> waiters;

        std::atomic<std::int32_t> result;  // 0 on success, negative errno from the daemon otherwise

        // Wall-clock ns past which a grant here means nothing, stamped when the daemon takes the turn
        // and left alone for the grants riding that acquire. Zero reads as expired.
        std::atomic<std::uint64_t> expiresAtNanos;
    } request;

    // ── the registry line ──────────────────────────────────────────
    // Written by the daemon, read by requesters resolving a name. alignas starts the second line here,
    // so a lookup never touches the words above.
    struct alignas(CachelineBytes)
    {
        char name[MaxName];

        // What this slot is. One word, so a scanner cannot catch a half-written name the way it
        // could if free meant blank bytes.
        std::atomic<DomainState> state;
    } registry;

    // ── the state machine ──────────────────────────────────────────
    // The store and the bump, never one without the other: a waiter reads seq before state, so a store
    // without the bump would start a sleep on a version that had already moved.
    void publish(RequestState next) noexcept
    {
        request.state.store(next, std::memory_order_seq_cst);
        request.seq.fetch_add(1, std::memory_order_release);
    }

    // seq_cst because each side stores its own word and then reads the other's, and a weaker pair lets
    // both conclude the turn is theirs. A seq_cst load is a plain load on this target.
    [[nodiscard]] RequestState getState() const noexcept
    {
        return request.state.load(std::memory_order_seq_cst);
    }

    // seq is read before state, so a store landing after the state read has already moved seq and the
    // sleep refuses to start. nullopt on the deadline; @settled says which states end the wait.
    template <typename T>
    [[nodiscard]] std::optional<RequestState>
    awaitState(timing::Nanos timeout, T&& settled, timing::Nanos spin = util::SpinBeforeSleep)
    {
        const timing::Deadline deadline{timeout};

        while (true)
        {
            const std::uint32_t seen = request.seq.load(std::memory_order_acquire);
            const RequestState seenState = request.state.load(std::memory_order_acquire);
            if (settled(seenState))
            {
                return seenState;
            }

            const auto remaining = deadline.remaining();
            if (!remaining)
            {
                return std::nullopt;
            }

            static_cast<void>(util::waitOnWord(request.seq, seen, *remaining, spin));
        }
    }

    // Only ever the requester holding this domain's mutex sleeps on that bump, so a requester
    // publishing its own state has nobody to wake and only the daemon calls this.
    void wakeRequester() noexcept
    {
        util::wakeAllWaiters(request.seq);
    }

    // ── the daemon's answer ────────────────────────────────────────
    // result before the state that sends a requester to read it. The other order hands it whatever the
    // last exchange left in the word.
    void publishGrant() noexcept
    {
        request.result.store(0, std::memory_order_release);
        publish(RequestState::LockHeld);
    }

    void publishFailure(std::int32_t failure) noexcept
    {
        request.result.store(failure, std::memory_order_release);
        publish(RequestState::Error);
    }

    // Meaningful only once an answer is published.
    [[nodiscard]] std::int32_t getFailureCode() const noexcept
    {
        return request.result.load(std::memory_order_acquire);
    }

    // ── the turn behind a grant ────────────────────────────────────
    // Zero is expired, so a fresh area and a revoked turn read the same and neither grants anything.
    [[nodiscard]] bool hasValidTurn() const noexcept
    {
        return timing::WallStamp{request.expiresAtNanos.load(std::memory_order_seq_cst)}.ahead().has_value();
    }

    // Stamped when the daemon takes the turn rather than when it answers, since taking it is the last
    // moment the region confirmed it was ours.
    void holdTurnUntil(std::uint64_t wallNanos) noexcept
    {
        request.expiresAtNanos.store(wallNanos, std::memory_order_seq_cst);
    }

    // From here no requester enters a section without asking. Ordered, because the daemon reads the
    // state after this while a requester stores that state and then reads this word.
    void revokeTurn() noexcept
    {
        request.expiresAtNanos.store(0, std::memory_order_seq_cst);
    }

    // ── local demand ───────────────────────────────────────────────
    // seq_cst for the same reason revokeTurn is: this is the read on one side of that store-then-read
    // pair, and a weaker one lets both sides conclude the turn is theirs.
    [[nodiscard]] bool hasWaiters() const noexcept
    {
        return request.waiters.load(std::memory_order_seq_cst) > 0;
    }

    // The right to be the one asking, not the CXL turn. The token falls when the acquire returns: a
    // wider scope would have the holder counted as queued.
    [[nodiscard]] util::RobustLock lockForRequest()
    {
        const util::OccupancyToken queued{request.waiters};
        util::RobustLock held{request.lock};
        recordAbandonment(held);
        return held;
    }

    // Both halves of "nobody on this node is using this domain": the mutex says no request is in
    // flight, the count says none is queued. nullopt has released the mutex, so it holds nothing.
    [[nodiscard]] std::optional<util::RobustLock> tryLockIfIdle()
    {
        auto held = util::RobustLock::tryLock(request.lock);
        if (held)
        {
            // Before the tests below, which can walk away from a mutex whose death nobody else will
            // ever be told about: the kernel gave that answer to this acquire alone.
            recordAbandonment(*held);
        }
        if (!held || getState() != RequestState::Idle || hasWaiters())
        {
            return std::nullopt;
        }

        return held;
    }

    // Whether a holder that died inside its section was cleared out of the way. The state word it left
    // says LockHeld and rings for nobody, so the mutex is the only witness the kernel offers.
    [[nodiscard]] bool reclaimAbandoned()
    {
        std::optional<util::RobustLock> held = util::RobustLock::tryLock(request.lock);
        if (!held)
        {
            // A live requester holds it, which is the answer either way: its own release ends this.
            return false;
        }

        recordAbandonment(*held);
        if (!held->wasAbandoned())
        {
            return false;
        }

        // The section is over because its holder is gone. Idle is what the state would have said had
        // that holder reached its own release.
        publish(RequestState::Idle);
        return true;
    }

    // Nonzero once a holder died inside its section, and stays so until a caller that repaired what
    // the dead holder guarded clears it. The mutex itself is sound by the time this can be read.
    [[nodiscard]] std::uint16_t getAbandonCount() const noexcept
    {
        return request.abandonCount.load(std::memory_order_acquire);
    }

    // Fails when a further death landed after @seen was read, so a caller learns its repair covered
    // only part of what is outstanding rather than clearing a count it never saw.
    [[nodiscard]] bool clearAbandonCount(std::uint16_t seen) noexcept
    {
        return request.abandonCount.compare_exchange_strong(seen, 0, std::memory_order_acq_rel);
    }

    // ── name and slot ──────────────────────────────────────────────
    // Takes a name and not a text, so nothing here can store one a requester would resolve to nothing
    // while the slot looks taken. Writes the whole field, so no stale byte survives.
    void setName(const DomainName& wanted) noexcept
    {
        wanted.write(registry.name);
    }

    // setName's partner, for a slot whose name will not be read again. Only after markFree, since a
    // live slot with no name resolves to nothing while still reading as taken.
    void clearName() noexcept
    {
        DomainName::clear(registry.name);
    }

    // A name that fills the field carries no terminator. Says nothing about live: a freed slot whose
    // name was not cleared still holds the bytes it had.
    [[nodiscard]] std::string_view getName() const noexcept
    {
        return DomainName::read(registry.name);
    }

    // A slot that is not Live matches nothing, whatever stale bytes its name field holds.
    [[nodiscard]] bool isNamed(std::string_view wanted) const noexcept
    {
        return isExpectedState(DomainState::Live) && getName() == wanted;
    }

    // The name field answers nothing on its own, so this word is what a scanner believes.
    [[nodiscard]] bool isExpectedState(DomainState expected) const noexcept
    {
        return registry.state.load(std::memory_order_acquire) == expected;
    }

    // One store, which is what lets that scanner trust this word and nothing else.
    void markState(DomainState next) noexcept
    {
        registry.state.store(next, std::memory_order_release);
    }

    // A helper of the acquires above and not part of the record's contract. Access control on a member
    // function leaves the layout alone, so the offsetof asserts below still hold.
private:
    // The kernel reports a dead holder to the first acquire after the death and to no other, so the
    // count is raised here rather than left in the lock object that is about to go out of scope.
    void recordAbandonment(const util::RobustLock& held) noexcept
    {
        if (held.wasAbandoned())
        {
            request.abandonCount.fetch_add(1, std::memory_order_release);
        }
    }
};

struct alignas(CachelineBytes) SharedArea_t
{
    // ── header ─────────────────────────────────────────────────────
    // What a joiner reads before it trusts a single offset below. First, and abiVersion first inside it,
    // so the gate word sits at offset 0 whatever a later version does to the rest.
    struct
    {
        // Stamped last with a release store, so it is both the ready flag and the compatibility check. A
        // fresh shm object reads as zero and is refused.
        std::atomic<std::uint32_t> abiVersion;

        // Plain, because the release/acquire pair on abiVersion orders them and nothing writes them again.
        std::uint32_t maxDomains;
        std::uint32_t areaBytes;
    } layout;

    // ── who is serving ─────────────────────────────────────────────
    // Grouped so the words about the daemon carry no prefix, and widest first so the group costs no
    // padding: the 8-byte word at its front is what its own alignment then asks for anyway.
    struct
    {
        // Whether the dispatcher is inside its doorbell sleep, set by the wait itself. Nothing acts on it:
        // knock() sends the wake unconditionally and a trace note is the only reader.
        std::atomic<std::uint32_t> dispatcherParked;

        // Futex word, bumped on every pending-bit set. A daemon must read it before draining and pass
        // that value to FUTEX_WAIT, or a request arriving mid-drain would not stop the sleep.
        std::atomic<std::uint32_t> doorbell;

        // Held for the daemon's whole run, and nothing tries it yet: a requester judges by the turn stamp
        // instead, which is the heuristic this answers exactly. Last, so the words above stay packed.
        pthread_mutex_t liveness;
    } daemon;

    // ── the domains ────────────────────────────────────────────────
    // The table's own alignment pads the bitmap out to a full line, which costs 56 bytes once and leaves
    // the word every requester read-modify-writes on a line nothing else shares.
    struct
    {
        // Which domains have work and not how many requests arrived: several requests for one domain
        // coalesce into one bit. Draining is the daemon's, since it consumes the bits a requester raised.
        bitmap::AtomicBits<MaxDomains> pending;

        Domain_t table[MaxDomains];
    } domain;

    // ── the header ─────────────────────────────────────────────────
    // The version goes last and with release, so a joiner that sees one sees the words above it.
    void publishReady(std::uint32_t bytes) noexcept
    {
        layout.maxDomains = MaxDomains;
        layout.areaBytes = bytes;
        layout.abiVersion.store(AbiVersion, std::memory_order_release);
    }

    // Zero for an area nothing has published. A joiner asks this before it trusts any offset below it.
    [[nodiscard]] std::uint32_t getAbiVersion() const noexcept
    {
        return layout.abiVersion.load(std::memory_order_acquire);
    }

    // What the writing build laid out, for a joiner to compare against its own sizeof.
    [[nodiscard]] std::uint32_t getAreaBytes() const noexcept
    {
        return layout.areaBytes;
    }

    // ── name lookup ────────────────────────────────────────────────
    // NoDomain when no live domain carries that name. Both sides ask: a requester turns a name into an
    // id, and the daemon checks a name is free before it creates one.
    [[nodiscard]] std::uint32_t resolve(std::string_view name) const noexcept
    {
        // Indexed rather than a range: the answer is the index, which a range-for would discard.
        for (std::uint32_t index = 0; index < MaxDomains; ++index)
        {
            if (domain.table[index].isNamed(name))
            {
                return index;
            }
        }

        return NoDomain;
    }

    // ── opening a request ──────────────────────────────────────────
    // Only LockRequested belongs here: the daemon's states are answers, and ringing for one wakes it to
    // look at a domain it has already dealt with. Both halves or neither.
    void request(std::uint32_t domainId, RequestState next) noexcept
    {
        domain.table[domainId].publish(next);
        ring(domainId);
    }

    // ── the doorbell ───────────────────────────────────────────────
    // The value, because a futex wait needs the word it will compare against rather than a verdict.
    [[nodiscard]] std::uint32_t readDoorbell() const noexcept
    {
        return daemon.doorbell.load(std::memory_order_acquire);
    }

    // A verdict and not the word, because nothing waits on this one.
    [[nodiscard]] bool isDispatcherParked() const noexcept
    {
        return daemon.dispatcherParked.load(std::memory_order_acquire) == 1;
    }

    // The bit first, then the knock. The doorbell says work exists and not which domain, so a daemon
    // woken before the bit was up would drain an empty bitmap and sleep again.
    void ring(std::uint32_t domainId) noexcept
    {
        if (domainId >= MaxDomains)
        {
            // Not a domain, so no bit goes up. Knocking now would wake the daemon to find nothing.
            return;
        }

        static_cast<void>(domain.pending.claim(domainId));
        knock();
    }

    // The knock alone, for a caller with nothing to point at. Reachable from a signal handler, since a
    // lock-free fetch_add and a futex syscall are all it does.
    void knock() noexcept
    {
        daemon.doorbell.fetch_add(1, std::memory_order_release);

        // One, because one thread serves domains and every other daemon thread waits on a word of its
        // own, so there is no wrong waiter to wake.
        util::wakeOneWaiter(daemon.doorbell);
    }
};

// The split the type is built around, which otherwise rests on pthread_mutex_t being 40 bytes here. A
// contended half one word larger pushes the registry line onto a third cacheline.
static_assert(sizeof(Domain_t) == 2 * std::size_t{CachelineBytes},
              "a domain is a contended line and a registry line");
static_assert(offsetof(Domain_t, registry) == CachelineBytes,
              "the registry line starts at the second one");

// The compatibility gate, at the one offset a joiner reads without knowing the layout. Anything ahead of
// it leaves a mismatched peer misreading the area instead of refusing it.
static_assert(offsetof(SharedArea_t, layout.abiVersion) == 0,
              "the version word opens the area");

// The header stays inside two lines, so a store to any word in it leaves the first domain's own lines
// alone. A field that pushed it past this would put header traffic on a domain a requester is using.
static_assert(offsetof(SharedArea_t, domain.table) == 2 * std::size_t{CachelineBytes},
              "the table opens the third line");

// A non-lock-free atomic routes through a lock table private to each process, so two processes would
// serialise against different locks and the shared word would tear in silence.
static_assert(std::atomic<std::uint16_t>::is_always_lock_free,
              "shared 16-bit words must be lock free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "shared 32-bit words must be lock free");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "shared 64-bit words must be lock free");
static_assert(std::atomic<RequestState>::is_always_lock_free,
              "shared state word must be lock free");
static_assert(std::atomic<DomainState>::is_always_lock_free,
              "shared slot word must be lock free");

}  // namespace cmed::protocol
