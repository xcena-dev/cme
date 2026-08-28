// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_area.hpp -- one cmed area owned by one probe, and the words a case writes by hand.
//
// The one place outside shared_area.hpp that names a field of protocol::SharedArea_t or protocol::Domain_t directly,
// for what a probe cannot get from the ABI type's own methods: a header a joiner would refuse, a
// count a requester earns, a raw word read to check the representation.
//
// An area is a memfd, so two probes never collide over a name and nothing outlives the process.

#pragma once

#include <fcntl.h>
#include <pthread.h>  // IWYU pragma: keep (declares pthread_mutex_t through bits/pthreadtypes.h)

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include "cmed/errors.hpp"
#include "cmed/robust_lock.hpp"
#include "common/bitmap.hpp"
#include "common/timing.hpp"
#include "daemon/startup/served_area.hpp"
#include "shared/area.hpp"
#include "shared/posix/unique_fd.hpp"
#include "shared/protocol/domain_name.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::harness
{

// Slot 0 is the control domain, so a data domain starts here.
constexpr std::uint32_t FirstDataSlot = 1;

// Gap between poll tries while a probe waits for something to come up. Not per-call: no case is
// about the interval itself.
constexpr timing::Millis ProbePoll{2};

// For a probe reaching a daemon a fixture has already started, where the wait never runs out.
constexpr timing::Millis ProbeAttachWait{2000};

// Owns one area for as long as the probe runs. Nothing to clean up: the memfd is gone when this
// mapping and this descriptor are.
class ProbeArea
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // @label reaches /proc only, so two probes may pass the same one.
    explicit ProbeArea(const char* label)
        : area_{daemon::formatArea(label)}
    {
    }

    ProbeArea(const ProbeArea&) = delete;
    ProbeArea(ProbeArea&&) = delete;
    ~ProbeArea() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────
    ProbeArea& operator=(const ProbeArea&) = delete;
    ProbeArea& operator=(ProbeArea&&) = delete;

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] protocol::SharedArea_t& shared() noexcept
    {
        return area_.shared();
    }

    // What a probe hands StubSetup, which passes it to whoever greets. The receiver maps its own
    // copy rather than sharing this mapping.
    [[nodiscard]] posix::FileDesc descriptor() const noexcept
    {
        return area_.descriptor();
    }

private:
    CmedArea area_;
};

// A second mapping of one area, for a case that reads the words another mapping is writing. Its own
// mapping rather than a reference to the fixture's, because that separation is what is being checked.
[[nodiscard]] inline CmedArea attachAgain(posix::FileDesc areaDescriptor)
{
    posix::UniqueFd copied{::fcntl(areaDescriptor, F_DUPFD_CLOEXEC, 0)};
    if (!copied)
    {
        throw CmedBackendError{"cmed::harness: cannot copy the area descriptor", lastSystemError()};
    }
    return CmedArea::attach(std::move(copied));
}

// ── the slot a case names ──────────────────────────────────────────────
// The domain table by index, so a slot number is the only thing a case spells and every question
// about the slot is then the domain's own method.
[[nodiscard]] inline protocol::Domain_t& resolveSlot(protocol::SharedArea_t& area, std::uint32_t domainId) noexcept
{
    return area.domain.table[domainId];
}

// The same slot off an area a case only reads, so asking a question about one costs it no write access.
[[nodiscard]] inline const protocol::Domain_t& resolveSlot(const protocol::SharedArea_t& area,
                                                           std::uint32_t domainId) noexcept
{
    return area.domain.table[domainId];
}

// ── the header a joiner checks ─────────────────────────────────────────
// formatArea always writes a true header, so these setters exist for a case that needs one a
// joiner would refuse.
inline void setAbiVersion(protocol::SharedArea_t& area, std::uint32_t version) noexcept
{
    area.layout.abiVersion.store(version, std::memory_order_release);
}

inline void setAreaBytes(protocol::SharedArea_t& area, std::uint32_t bytes) noexcept
{
    area.layout.areaBytes = bytes;
}

// No joiner checks this word, so it has no method: what a mapping refuses is the size, and this says
// only what the writing build was compiled against.
[[nodiscard]] inline std::uint32_t getMaxDomains(const protocol::SharedArea_t& area) noexcept
{
    return area.layout.maxDomains;
}

// ── the doorbell, as a count ───────────────────────────────────────────
// The value and not "has it rung": a case asserts a call knocked exactly once, which takes the word
// before and after. A futex wait keyed on this word needs the word itself and not a reading of it.
[[nodiscard]] inline std::uint32_t getDoorbell(const protocol::SharedArea_t& area) noexcept
{
    return area.daemon.doorbell.load(std::memory_order_acquire);
}

// A knock count a fresh area could not have reached, so a case can tell one area's word from another's.
inline void setDoorbell(protocol::SharedArea_t& area, std::uint32_t knocks) noexcept
{
    area.daemon.doorbell.store(knocks, std::memory_order_release);
}

// The atomic and not a value, for a case that sleeps on the knock rather than reading it. A futex wait
// keys on the address, which no reading of the word can stand in for.
[[nodiscard]] inline std::atomic<std::uint32_t>& resolveDoorbellWord(protocol::SharedArea_t& area) noexcept
{
    return area.daemon.doorbell;
}

// ── the pending bitmap ─────────────────────────────────────────────────
// A bit without the knock ring() sends with it, which is how a case arranges a bitmap the drain has
// not been told about. Both answer false for an id past the table.
[[nodiscard]] inline bool setPendingBit(protocol::SharedArea_t& area, std::uint32_t domainId) noexcept
{
    if (domainId >= cmed::MaxDomains)
    {
        return false;
    }
    static_cast<void>(area.domain.pending.claim(domainId));
    return true;
}

[[nodiscard]] inline bool clearPendingBit(protocol::SharedArea_t& area, std::uint32_t domainId) noexcept
{
    if (domainId >= cmed::MaxDomains)
    {
        return false;
    }
    static_cast<void>(area.domain.pending.release(domainId));
    return true;
}

// The word rather than one bit, because a case asserting that exactly one bit went up has to see the
// others that did not.
[[nodiscard]] inline std::uint64_t getPendingWord(const protocol::SharedArea_t& area, std::uint32_t wordIndex) noexcept
{
    return area.domain.pending.takeSnapshot().getWord(wordIndex);
}

// Every bit taken and cleared, the way one pass of a drain consumes them.
[[nodiscard]] inline bitmap::Bits<cmed::MaxDomains> takeEveryPendingBit(protocol::SharedArea_t& area) noexcept
{
    return area.domain.pending.takeAll();
}

inline void clearEveryPendingBit(protocol::SharedArea_t& area) noexcept
{
    static_cast<void>(area.domain.pending.takeAll());
}

// Every bit still up, ORed into one word, for a case whose assertion is that none is.
[[nodiscard]] inline std::uint64_t getEveryPendingBit(const protocol::SharedArea_t& area) noexcept
{
    const bitmap::Bits<cmed::MaxDomains> raised = area.domain.pending.takeSnapshot();
    std::uint64_t seen = 0;
    for (std::uint32_t wordIndex = 0; wordIndex < raised.WordCount; ++wordIndex)
    {
        seen |= raised.getWord(wordIndex);
    }
    return seen;
}

// ── the domain words nothing on the domain writes ──────────────────────
// The state without the version bump publish() pairs with it. That pairing is what a waiter's sleep
// rests on, so a case about it needs the half publish() will not do on its own.
inline void setState(protocol::Domain_t& domain, protocol::RequestState next) noexcept
{
    domain.request.state.store(next, std::memory_order_release);
}

// The version publish() bumps, as a value, because what says the bump happened is that this word
// differs from what it read before the call.
[[nodiscard]] inline std::uint32_t getSeq(const protocol::Domain_t& domain) noexcept
{
    return domain.request.seq.load(std::memory_order_acquire);
}

// The count and not whether anything is queued. A case waits for exactly one requester to be blocked
// on the mutex, and hasWaiters() cannot say when the second has arrived.
[[nodiscard]] inline std::uint32_t getWaiters(const protocol::Domain_t& domain) noexcept
{
    return domain.request.waiters.load(std::memory_order_acquire);
}

// Planted rather than earned. The count is a requester's own word, raised while it blocks on the
// domain mutex, and a case that spawned a requester to raise it would be timing a thread instead.
inline void setWaiters(protocol::Domain_t& domain, std::uint32_t queued) noexcept
{
    domain.request.waiters.store(queued, std::memory_order_release);
}

// The word publishGrant and publishFailure write, given a value that is neither. A case reads it back
// with getFailureCode to say a write through one mapping landed in another.
inline void setResult(protocol::Domain_t& domain, std::int32_t value) noexcept
{
    domain.request.result.store(value, std::memory_order_release);
}

// ── the words a slot lends out ─────────────────────────────────────────
// The raw slot, for memory a probe shares with a process it forked. The atomic itself, not a value:
// a counter moved across processes is read-modify-written, and a futex wait keys on the address.
[[nodiscard]] inline std::atomic<std::uint32_t>& resolveWaitersWord(protocol::SharedArea_t& area,
                                                                    std::uint32_t domainId) noexcept
{
    return area.domain.table[domainId].request.waiters;
}

[[nodiscard]] inline std::atomic<std::uint32_t>& resolveSeqWord(protocol::SharedArea_t& area,
                                                                std::uint32_t domainId) noexcept
{
    return area.domain.table[domainId].request.seq;
}

[[nodiscard]] inline std::atomic<std::int32_t>& resolveResultWord(protocol::SharedArea_t& area,
                                                                  std::uint32_t domainId) noexcept
{
    return area.domain.table[domainId].request.result;
}

// The mutex itself, for two questions asked from outside a request: whether it is free, and what
// a dead holder left in it. Neither is a method on the domain.
[[nodiscard]] inline pthread_mutex_t& resolveDomainMutex(protocol::SharedArea_t& area, std::uint32_t domainId) noexcept
{
    return area.domain.table[domainId].request.lock;
}

// Writes the registry half, which is what the real daemon does in createDomain. setName writes the
// whole field, so a name that fills it carries no terminator and one shorter carries no stale bytes.
inline void publishDomain(protocol::SharedArea_t& area, std::uint32_t domainId, std::string_view name)
{
    protocol::Domain_t& domain = resolveSlot(area, domainId);
    if (const auto named = protocol::DomainName::make(name))
    {
        domain.setName(*named);
    }
    domain.markState(protocol::DomainState::Live);
}

// Several names at once, from @firstId upwards. The order matters: a lookup scans upward, so
// which of two similar names sits lower decides what a lenient comparison answers.
inline void
publishDomains(protocol::SharedArea_t& area, std::uint32_t firstId, std::initializer_list<std::string_view> names)
{
    std::uint32_t domainId = firstId;
    for (const std::string_view name : names)
    {
        publishDomain(area, domainId, name);
        ++domainId;
    }
}

[[nodiscard]] inline protocol::RequestState readState(protocol::SharedArea_t& area, std::uint32_t domainId) noexcept
{
    return resolveSlot(area, domainId).getState();
}

[[nodiscard]] inline bool isHeld(protocol::SharedArea_t& area, std::uint32_t domainId) noexcept
{
    return readState(area, domainId) == protocol::RequestState::LockHeld;
}

[[nodiscard]] inline bool isIdle(protocol::SharedArea_t& area, std::uint32_t domainId) noexcept
{
    return readState(area, domainId) == protocol::RequestState::Idle;
}

// Asked from another thread: a robust mutex is not recursive, so the owner asking about its own
// mutex would be a different question. tryLock, so a mutex a case leaked fails red, not a hang.
[[nodiscard]] inline bool isMutexFree(pthread_mutex_t& mutex)
{
    bool taken = false;
    std::thread asker{[&mutex, &taken]
                      {
                          const std::optional<util::RobustLock> held = util::RobustLock::tryLock(mutex);
                          taken = held.has_value();
                      }};
    asker.join();
    return taken;
}

// What a dead holder leaves: the mutex is this caller's now, and it says the previous holder did
// not release it. Reading that consumes it, since taking an EOWNERDEAD lock makes it consistent.
[[nodiscard]] inline bool wasLeftAbandoned(pthread_mutex_t& mutex)
{
    const std::optional<util::RobustLock> inherited = util::RobustLock::tryLock(mutex);
    return inherited.has_value() && inherited->wasAbandoned();
}

}  // namespace cmed::harness
