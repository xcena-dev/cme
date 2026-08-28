// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/domain/manager.cpp -- see manager.hpp.

#include "daemon/domain/manager.hpp"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "cmed/robust_lock.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "daemon/domain/local_domain.hpp"
#include "daemon/observe/counters.hpp"
#include "daemon/observe/failpoint.hpp"
#include "daemon/startup/config.hpp"
#include "shared/protocol/domain_name.hpp"
#include "shared/protocol/message.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::daemon
{

namespace
{

// Published into the state machine's result word rather than a reply, and read there as a negative
// errno. cme's failures are types rather than codes, and the requester only asks whether to retry.
enum class GrantRefusal : std::int32_t
{
    TurnUnavailable = -EBUSY,
    DomainUnusable = -EINVAL,
};

void refuse(protocol::Domain_t& domain, GrantRefusal failure) noexcept
{
    domain.publishFailure(static_cast<std::int32_t>(failure));
    domain.wakeRequester();
}

// Best effort. A leave that failed leaves this node a participant, and what reports that is the next
// delete rather than this path: nothing here has anyone to tell.
void leaveDomain(cme::Session& session, const protocol::DomainName& name) noexcept
{
    try
    {
        session.leaveDomain(name.getText());
    }
    catch (const std::exception&)
    {
        // @expected: best-effort leave; nothing here has anyone to report the failure to.
    }
}

// The turn's own leave. By handle, since the slot may carry another domain by now and this one owes
// its leave to the domain it joined. Incarnation zero is a turn never taken.
void leaveDomain(cme::Session& session, cme::DomainHandle_t handle) noexcept
{
    if (handle.incarnation == 0)
    {
        return;
    }

    try
    {
        session.leaveDomain(handle);
    }
    catch (const std::exception&)
    {
        // @expected: best-effort leave; nothing here has anyone to report the failure to.
    }
}

}  // namespace

LocalDomain* DomainManager::findLocalDomain(std::uint32_t domainId) noexcept
{
    return domainId < MaxDomains ? &domains_.entries[domainId] : nullptr;
}

LocalDomain& DomainManager::takeLocalDomain(std::uint32_t domainId) noexcept
{
    return domains_.entries[domainId];
}

DomainManager::DomainManager(cme::Session& session, protocol::SharedArea_t& area,
                             const DaemonConfig_t& config)
    : session_{&session},
      config_{.daemon = &config, .refresh = timing::Deadline{timing::Millis::zero()}},
      domains_{.area = &area, .naming = {}, .ids = {}, .entries = std::make_unique<LocalDomain[]>(MaxDomains)}
{
    // Paired once, since neither half of a domain ever moves: an id is the subscript into both.
    for (std::uint32_t domainId = 0; domainId < MaxDomains; ++domainId)
    {
        takeLocalDomain(domainId).pairSlot(domainId, domains_.area->domain.table[domainId]);
    }

    refresh();
}

// Out of line, since destroying the table needs the definition above.
DomainManager::~DomainManager() noexcept = default;

DomainAnswer DomainManager::serveCommand(protocol::MessageType asked, std::string_view asking) noexcept
{
    // The one place a peer's bytes become a name: past it nothing below asks the question again.
    const auto made = protocol::DomainName::make(asking);
    if (!made)
    {
        return DomainAnswer::NameUnusable;
    }

    const protocol::DomainName& name = *made;
    switch (asked)
    {
        case protocol::MessageType::CreateDomain:
            return create(name);
        case protocol::MessageType::DeleteDomain:
            return remove(name);
        case protocol::MessageType::JoinDomain:
            return join(name);
        case protocol::MessageType::LeaveDomain:
            return leave(name);
        default:
            // Unreachable: the control loop admits a message on the same test this switch makes.
            return DomainAnswer::NameUnusable;
    }
}

DomainAnswer DomainManager::create(const protocol::DomainName& name) noexcept
{
    cme::DomainHandle_t created{0, 0};
    try
    {
        created = session_->createDomain(name.getText());
    }
    catch (const cme::DomainExistsError&)
    {
        // Idempotent from here: the name is in the region and the caller wants to be in it, which is
        // what create already leaves behind on the way through. The join answers the same handle.
        try
        {
            created = session_->joinDomain(name.getText());
        }
        catch (const cme::UnknownDomainError&)
        {
            // Deleted between the two calls, so the name is free again and asking is what finds that.
            return DomainAnswer::DomainBroken;
        }
        catch (const std::exception&)
        {
            return DomainAnswer::RegionUnusable;
        }
    }
    catch (const cme::DomainLimitError&)
    {
        return DomainAnswer::NoRoom;
    }
    catch (const std::exception&)
    {
        return DomainAnswer::RegionUnusable;
    }

    // The region has it now. A registry with no free slot leaves it lockable by another node and not by
    // this one, which is what the answer says rather than hiding.
    DomainAnswer claimed = DomainAnswer::Served;
    {
        const std::lock_guard<std::mutex> naming{domains_.naming};
        claimed = claim(name, created);
    }

    // Either call above made this peer a participant, so the count is all that is new. Left uncounted,
    // the first hand-back on this slot would end a domain the creator still means to use.
    if (claimed == DomainAnswer::Served)
    {
        const LocalDomain::Guard held{takeLocalDomain(created.id)};
        held->addJoin();
        return DomainAnswer::Served;
    }

    // The region has the domain and this peer is in it, so undoing that would leave a domain nothing
    // here can reach or delete. What claim could not publish, a refresh publishes on its own pass.
    return claimed;
}

DomainAnswer DomainManager::remove(const protocol::DomainName& name) noexcept
{
    const std::lock_guard<std::mutex> naming{domains_.naming};
    const std::optional<std::uint32_t> found = domains_.ids.find(name);
    if (!found)
    {
        return DomainAnswer::NoSuchDomain;
    }
    LocalDomain& target = takeLocalDomain(*found);
    protocol::Domain_t& domain = target.slot();

    // Delete is the exit for the last participant; a node outside the domain has none to make,
    // and joining then asking again is the caller's fix.
    {
        const LocalDomain::Guard held{target};
        if (!held->keepsParticipation())
        {
            return DomainAnswer::NotParticipating;
        }
    }

    // No requester that has not already resolved the name gets past this. The ones that have are
    // what the wait below is for.
    domain.markState(protocol::DomainState::Free);

    // The gap between looks. Short, because what is being waited on is a critical section ending
    // rather than a timer.
    constexpr timing::Millis QuiescePoll{1};

    // A requester already queued on the mutex resolved the id before the name cleared, so deleting
    // under it would strand that request. Nothing is held across the looks, which would block it.
    const std::optional<util::RobustLock> quiet =
        poll::awaitValue([&domain]
                         {
                             return domain.tryLockIfIdle();
                         },
                         config_.daemon->registry.deleteWaitTimeout, QuiescePoll);
    if (!quiet)
    {
        domain.markState(protocol::DomainState::Live);
        return DomainAnswer::StillInUse;
    }

    // The turn goes back without leaving the domain: cme takes the turn itself inside the delete, and
    // one peer holding two Guards for one domain would drop the first one's ownership under the second.
    {
        const LocalDomain::Guard held{target};
        held->releaseTurn();
        domain.revokeTurn();
    }

    try
    {
        session_->deleteDomain(name.getText());
    }
    catch (const cme::UnknownDomainError&)
    {
        // Gone from the region already, so the slot standing empty is the state both sides agree
        // on and the answer is what the caller asked for.
        retire(target, name);
        return DomainAnswer::Served;
    }
    catch (const cme::NotParticipatingError&)
    {
        // A peer on another node participates. Its own answer, because a caller can act on it:
        // that node leaves and the delete is asked again.
        domain.markState(protocol::DomainState::Live);
        return DomainAnswer::ParticipantsRemain;
    }
    catch (const std::exception&)
    {
        // The region kept it, so this node has to be able to reach it again. The next grant
        // takes the turn back, and the participation this table counts was never dropped.
        domain.markState(protocol::DomainState::Live);
        return DomainAnswer::RegionUnusable;
    }

    retire(target, name);
    return DomainAnswer::Served;
}

// The name goes with the domain. A slot left carrying one it no longer has resolves for a requester
// that then finds nothing there, and the next delete of that name answers the wrong refusal.
void DomainManager::retire(LocalDomain& target, const protocol::DomainName& name) noexcept
{
    target.slot().clearName();
    domains_.ids.clear(name, target.readId());

    const LocalDomain::Guard held{target};
    held->clearJoins();
}

// The two run on one thread and at their own paces: reading the region is a scan of its records and
// waits out an interval, while an expired hold has to go back inside one cohort window.
void DomainManager::maintain(const std::function<bool(std::uint32_t)>& isBusy) noexcept
{
    refresh();
    dropExpiredTurns(isBusy);
}

void DomainManager::refresh() noexcept
{
    if (!config_.refresh.expired())
    {
        return;
    }
    config_.refresh = timing::Deadline{config_.daemon->registry.refreshInterval};

    // Taken before the region is asked, not after. A name claimed while the region was answering is
    // absent from an answer that predates it, and the retiring below would free the slot it just took.
    const std::lock_guard<std::mutex> naming{domains_.naming};

    std::vector<cme::DomainEntry_t> living;
    try
    {
        living = session_->getDomainEntries();
    }
    catch (...)
    {
        // The region did not answer. Every name already in the table is still the reading each
        // requester holds, so leaving it alone is the only answer that cannot be wrong.
        observe_.bump(observe::DomainEvent::RefreshesLost);
        return;
    }

    // One bit per slot, since a slot number is the region's own domain id and the answer carries it.
    // A live slot the answer does not confirm at its own number is stale, whether by a name the region
    // dropped or by a name that moved to another id.
    static_assert(MaxDomains <= 64, "one word no longer covers the table");
    std::uint64_t confirmed = 0;
    for (const cme::DomainEntry_t& entry : living)
    {
        if (entry.handle.id >= MaxDomains)
        {
            continue;
        }

        // A Broken slot still carries the name it is being taken from, so only its id can confirm it.
        // The claim below is what puts this name there, and freeing it first would lose the mark.
        const protocol::Domain_t& slot = takeLocalDomain(entry.handle.id).slot();
        if (slot.isNamed(entry.name) || slot.isExpectedState(protocol::DomainState::Broken))
        {
            confirmed |= std::uint64_t{1} << entry.handle.id;
        }
    }

    // Retiring comes first. A name that left the region and came back gets a free slot that way rather
    // than being refused for one its own stale entry is holding.
    for (std::uint32_t slot = 0; slot < MaxDomains; ++slot)
    {
        protocol::Domain_t& domain = takeLocalDomain(slot).slot();

        // Broken as well as Live: a slot marked by a create the region has since dropped is one only
        // this sweep can free, because nothing scans for a name that is not there to be found.
        const bool taken = domain.isExpectedState(protocol::DomainState::Live) ||
                           domain.isExpectedState(protocol::DomainState::Broken);
        if (!taken || (confirmed & (std::uint64_t{1} << slot)) != 0)
        {
            continue;
        }

        // Only while nothing is using it. A slot freed under a request in flight would have its name
        // cleared out from under a requester that had already resolved the id.
        const std::optional<util::RobustLock> held = domain.tryLockIfIdle();
        if (!held)
        {
            continue;
        }

        // Read before the name goes, since that is what names the entry to drop.
        const auto retired = protocol::DomainName::make(domain.getName());
        domain.markState(protocol::DomainState::Free);
        domain.clearName();
        if (retired)
        {
            domains_.ids.clear(*retired, slot);
        }
        observe_.bump(observe::DomainEvent::SlotsRetired);
    }

    for (const cme::DomainEntry_t& entry : living)
    {
        if (const auto living = protocol::DomainName::make(entry.name))
        {
            static_cast<void>(claim(*living, entry.handle));
        }
    }
}

DomainAnswer DomainManager::claim(const protocol::DomainName& name, cme::DomainHandle_t handle) noexcept
{
    if (handle.id >= MaxDomains)
    {
        return DomainAnswer::NameUnusable;
    }

    protocol::Domain_t& domain = takeLocalDomain(handle.id).slot();

    // The id is the region's, so a live slot under this name is this domain rather than a second slot
    // for it. Answered before anything is written, since a name a scanner trusts is not rewritten.
    if (domain.isNamed(name.getText()))
    {
        domains_.ids.set(name, handle.id);
        return DomainAnswer::Served;
    }

    // The region gave this id to another name since, so what stands here is not a domain any more.
    // Broken and not Free: a daemon that stops after this leaves the next one a slot to recover.
    if (domain.isExpectedState(protocol::DomainState::Live))
    {
        domain.markState(protocol::DomainState::Broken);
        observe_.bump(observe::DomainEvent::SlotsBroken);
    }

    // Only while nothing is using it: a requester mid-request owns this slot, and the name would go
    // out from under it. The slot stays Broken until some pass finds it idle.
    if (domain.isExpectedState(protocol::DomainState::Broken))
    {
        const std::optional<util::RobustLock> held = domain.tryLockIfIdle();
        if (!held)
        {
            return DomainAnswer::DomainBroken;
        }
        domain.markState(protocol::DomainState::Free);
    }

    domain.setName(name);

    // The name before live, and live with release. A requester scanning sees a slot that is not a
    // domain until the whole name is there.
    domain.markState(protocol::DomainState::Live);
    domains_.ids.set(name, handle.id);
    return DomainAnswer::Served;
}

// The doorbell carries a number, so this is where one becomes a domain. Nothing below here takes an
// id: a domain knows its own, and only the requester and the workers ask in that language.
void DomainManager::serveLock(std::uint32_t domainId) noexcept
{
    LocalDomain* target = findLocalDomain(domainId);
    if (target == nullptr)
    {
        return;
    }

    const LocalDomain::Guard held{*target};

    switch (held->slot().getState())
    {
        case protocol::RequestState::LockRequested:
            grant(held);
            break;
        case protocol::RequestState::Idle:
            // A holder left with nothing queued behind it, or a request timed out; either way the
            // turn's fate is worth reconsidering.
            dropTurnIfIdle(held);
            break;
        default:
            // Granting, LockHeld, Error: nothing to do. A domain can wake for a bit raised before a
            // state the requester has since moved on from.
            break;
    }
}

void DomainManager::dropExpiredTurns(const std::function<bool(std::uint32_t)>& isBusy) noexcept
{
    for (std::uint32_t domainId = 0; domainId < MaxDomains; ++domainId)
    {
        // A worker holds this slot, and it may be about to store the guard this would drop.
        if (isBusy(domainId))
        {
            continue;
        }

        const LocalDomain::Guard held{takeLocalDomain(domainId)};
        if (!held->hasTurn() || !held->runExpired())
        {
            continue;
        }

        // First, because a holder that died inside its section leaves LockHeld standing and dropTurn
        // reads that as a live section it must not touch. Nothing else ever rings for that slot.
        static_cast<void>(held->slot().reclaimAbandoned());

        // A requester inside its section keeps the turn one more pass, which is dropTurn's answer and
        // not this sweep's judgement to make.
        static_cast<void>(dropTurn(held));
    }
}

DomainAnswer DomainManager::join(const protocol::DomainName& name) noexcept
{
    std::optional<std::uint32_t> found;
    {
        const std::lock_guard<std::mutex> naming{domains_.naming};
        found = domains_.ids.find(name);
    }
    if (!found)
    {
        return DomainAnswer::NoSuchDomain;
    }
    const LocalDomain::Guard held{takeLocalDomain(*found)};

    try
    {
        // Idempotent in libcme, so this asks every time rather than keeping a second copy of the fact
        // that it already has.
        session_->joinDomain(name.getText());
    }
    catch (const cme::UnknownDomainError&)
    {
        return DomainAnswer::NoSuchDomain;
    }
    catch (const std::exception&)
    {
        return DomainAnswer::RegionUnusable;
    }

    held->addJoin();
    return DomainAnswer::Served;
}

DomainAnswer DomainManager::leave(const protocol::DomainName& name) noexcept
{
    std::optional<std::uint32_t> found;
    {
        const std::lock_guard<std::mutex> naming{domains_.naming};
        found = domains_.ids.find(name);
    }
    if (!found)
    {
        return DomainAnswer::NoSuchDomain;
    }
    const LocalDomain::Guard held{takeLocalDomain(*found)};

    // Leaving what was never joined is what a caller asked for, not a failure.
    if (!held->dropJoin())
    {
        return DomainAnswer::Served;
    }

    // The turn keeps the participation while it is held, and the hand-back is what ends it. Leaving now
    // would drop a participation the guard above is using.
    if (!held->keepsParticipation())
    {
        leaveDomain(*session_, name);
    }
    return DomainAnswer::Served;
}

// The hand-back itself. False when a requester is inside a section riding this turn, which leaves the
// turn here for one more pass and is not a failure.
bool DomainManager::dropTurn(const LocalDomain::Guard& held) noexcept
{
    protocol::Domain_t& domain = held->slot();

    // First, and its own step: the read below is only sound because it precedes it, since a requester
    // stores its state and then reads the word this clears.
    domain.revokeTurn();

    // A requester is inside a section that rode this turn, so the turn is not this pass's to hand back.
    // Its release rings again, and the store above means whoever comes after it asks.
    if (domain.getState() == protocol::RequestState::LockHeld)
    {
        return false;
    }

    failpoint::reach(failpoint::Boundary::DropBeforeRelease);

    const std::uint64_t dropping = timing::wall<timing::Nanos>();
    held->releaseTurn();
    spent_.add(observe::SpanEvent::TurnDrop, timing::Nanos{timing::wall<timing::Nanos>() - dropping});
    observe_.bump(observe::DomainEvent::TurnsDropped);

    // The peer leaves with it too, unless a requester asked to keep this domain: a node that never
    // left would keep every other node from deleting it.
    if (!held->keepsParticipation())
    {
        leaveDomain(*session_, held->turnHandle());
    }
    return true;
}

// Idle here is nothing local queued, or this run's time up. Kept otherwise, which is what makes the
// next grant cost no cme acquire. Nothing to answer: a holder that left is already gone.
void DomainManager::dropTurnIfIdle(const LocalDomain::Guard& held) noexcept
{
    if (!held->hasTurn() || (held->slot().hasWaiters() && !held->runExpired()))
    {
        return;
    }

    static_cast<void>(dropTurn(held));
}

void DomainManager::grant(const LocalDomain::Guard& held) noexcept
{
    protocol::Domain_t& domain = held->slot();

    // Woken on Granting as well as on the answer: a requester spinning before it sleeps reads the state
    // it is waiting on, and this one tells it the daemon has its request rather than nothing at all.
    domain.publish(protocol::RequestState::Granting);
    domain.wakeRequester();

    try
    {
        // A turn already held keeps this peer in the domain, and cme refuses a delete while any peer
        // is in it. So the slot cannot have changed hands under a turn, and the name is asked for once.
        if (!held->hasTurn())
        {
            const std::string name{domain.getName()};

            // Idempotent in libcme, so the daemon keeps no record of having joined. A second copy
            // of that fact is one that can disagree with the first. The join answers the handle it
            // resolved to join by, which is the one to acquire on.
            const cme::DomainHandle_t resolved = session_->joinDomain(name);

            // Throws UnknownDomainError when the slot has been reused since, which the catch below turns
            // into the refusal a requester can act on.
            const std::uint64_t asking = timing::wall<timing::Nanos>();
            cme::Guard taken = session_->lock(resolved);
            spent_.add(observe::SpanEvent::RemoteAcquire, timing::Nanos{timing::wall<timing::Nanos>() - asking});

            failpoint::reach(failpoint::Boundary::AcquireBeforeRecord);
            held->takeTurn(std::move(taken), resolved, config_.daemon->cohort.hold);

            // The acquire is the last moment the region confirmed this peer holds the domain, so it is
            // what the grants riding it are dated from. Every grant in this run reads the same instant.
            domain.holdTurnUntil(timing::wallAfter(config_.daemon->cohort.grantValidity));
            observe_.bump(observe::DomainEvent::Acquires);
        }
    }
    catch (const cme::LockTimeoutError&)
    {
        observe_.bump(observe::DomainEvent::RefusedNoTurn);
        refuse(domain, GrantRefusal::TurnUnavailable);
        return;
    }
    catch (const std::exception&)
    {
        // An unknown name, a domain this peer never joined, a region that stopped answering. The
        // requester cannot retry its way out of any of them.
        observe_.bump(observe::DomainEvent::RefusedUnusable);
        refuse(domain, GrantRefusal::DomainUnusable);
        return;
    }

    observe_.bump(observe::DomainEvent::Grants);

    failpoint::reach(failpoint::Boundary::GrantBeforePublish);
    domain.publishGrant();
    failpoint::reach(failpoint::Boundary::GrantBeforeWake);
    domain.wakeRequester();
}

}  // namespace cmed::daemon
