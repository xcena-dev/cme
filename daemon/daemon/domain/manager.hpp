// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/domain/manager.hpp -- the domain names a requester can ask for, and the commands that change
// them. Only the daemon writes that table, so one owner holds both: a command and a refresh choose
// slots from the same pool, and a chooser that did not know about the other would hand out two.

#pragma once

#include <cerrno>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "daemon/domain/local_domain.hpp"
#include "daemon/observe/counters.hpp"
#include "daemon/startup/config.hpp"
#include "shared/protocol/domain_name.hpp"
#include "shared/protocol/id_lookup.hpp"
#include "shared/protocol/message.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::daemon
{

// What a control request is answered with. The requester reads it as a negative errno off the reply
// word, and Served is the zero that word carries on success.
enum class DomainAnswer : std::int32_t
{
    Served = 0,

    NameUnusable = -EINVAL,
    NameTaken = -EEXIST,
    NoRoom = -ENOSPC,
    NoSuchDomain = -ENOENT,
    StillInUse = -EBUSY,

    // Apart from StillInUse: that one is answered by waiting out a mid-flight local request, but this
    // one names a peer on another node whose own leave is the only thing that ends it.
    ParticipantsRemain = -ENOTEMPTY,

    // The region has the domain and this node's slot for it does not, so the two need reconciling
    // before anything here may use the name. Asking again is what mends it.
    DomainBroken = -EAGAIN,

    // Nobody on this node is in the domain, so there is nothing here to delete it with.
    NotParticipating = -EPERM,
    RegionUnusable = -EIO,
};

// An id never moves while live -- it is the bit a requester sets and the subscript the answer
// addresses -- so moving it would wake the wrong waiter.
class DomainManager
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // Reads the region once here, so no caller can start serving on a table that resolves nothing.
    // registryRefreshInterval bounds every pass after this one, since one pass scans the region's records.
    // Throws only what the table costs to build; every later call answers rather than throws.
    DomainManager(cme::Session& session, protocol::SharedArea_t& area, const DaemonConfig_t& config);

    DomainManager(const DomainManager&) = delete;
    DomainManager(DomainManager&&) = delete;
    ~DomainManager() noexcept;

    // ── operator= ──────────────────────────────────────────────────
    DomainManager& operator=(const DomainManager&) = delete;
    DomainManager& operator=(DomainManager&&) = delete;

    // ── the control thread ─────────────────────────────────────────
    // One request by its type: create, delete, join or leave. Never throws, since the answer travels
    // as a message rather than up a stack.
    [[nodiscard]] DomainAnswer serveCommand(protocol::MessageType asked, std::string_view asking) noexcept;

    // ── a worker thread ────────────────────────────────────────────
    // Takes the turn for @domainId and grants it, or reconsiders a turn nothing is queued on.
    void serveLock(std::uint32_t domainId) noexcept;

    // ── the maintenance thread ─────────────────────────────────────
    // Everything no requester rings for. @isBusy names domains a worker already holds, which this
    // leaves alone: that worker may be about to store the very guard a hand-back would drop.
    void maintain(const std::function<bool(std::uint32_t)>& isBusy) noexcept;

    // ── accessors ──────────────────────────────────────────────────
    // One event at a time, since a run is judged by how the counts move against each other rather than
    // by any one of them being consistent with the rest at an instant.
    [[nodiscard]] std::uint64_t readCount(observe::DomainEvent asked) const noexcept
    {
        return observe_.read(asked);
    }

    // What one of those cost on average. Apart from the counts because a run is judged by the two
    // together: acquires that stay flat while this climbs is a region answering more slowly.
    [[nodiscard]] std::uint64_t readMeanNanos(observe::SpanEvent asked) const noexcept
    {
        return spent_.readMeanNanos(asked);
    }

private:
    // ── on whichever thread asks ───────────────────────────────────
    // Where an id from outside becomes a domain, and the only place that judges one: null for an id
    // the table has no entry for. Everything past here says which domain by holding one.
    [[nodiscard]] LocalDomain* findLocalDomain(std::uint32_t domainId) noexcept;

    // The same by reference, for an id a range check has already passed. Undefined past the table, so
    // a caller that cannot show its id is in range asks findLocalDomain instead.
    [[nodiscard]] LocalDomain& takeLocalDomain(std::uint32_t domainId) noexcept;

    // Publishes @name at the entry @handle names, so this table's id is the region's own. Called by a
    // create and by a refresh, which is why it takes the naming mutex from neither: both hold it.
    [[nodiscard]] DomainAnswer claim(const protocol::DomainName& name, cme::DomainHandle_t handle) noexcept;

    // ── the control thread ─────────────────────────────────────────
    // The region first, then the table. A name published locally before cme accepted it would be
    // lockable here and nowhere else on the node's behalf.
    [[nodiscard]] DomainAnswer create(const protocol::DomainName& name) noexcept;

    // Local requesters first, then cme, then the table. The slot's live word goes down before the
    // wait so no further requester resolves the name, and back up if the delete does not happen.
    [[nodiscard]] DomainAnswer remove(const protocol::DomainName& name) noexcept;

    // Participation held for as long as a requester asks, rather than for one turn. Counted per
    // domain, because one peer here fronts every requester on the node.
    [[nodiscard]] DomainAnswer join(const protocol::DomainName& name) noexcept;
    [[nodiscard]] DomainAnswer leave(const protocol::DomainName& name) noexcept;

    // What a landed delete leaves behind: the slot's name, this daemon's index entry, and the count of
    // requesters that asked for it. Call under the naming mutex.
    void retire(LocalDomain& target, const protocol::DomainName& name) noexcept;

    // ── a worker thread ────────────────────────────────────────────
    // Buys the turn, or rides one this node already holds. The second costs no cme acquire, which is
    // the whole of cohorting.
    void grant(const LocalDomain::Guard& held) noexcept;

    // Idle here is nothing local queued, or this run's time up. Kept otherwise, which is what makes
    // the next grant cost no cme acquire.
    void dropTurnIfIdle(const LocalDomain::Guard& held) noexcept;

    // The hand-back itself. False when a requester is inside a section riding this turn, which leaves
    // the turn here for one more pass and is not a failure.
    [[nodiscard]] bool dropTurn(const LocalDomain::Guard& held) noexcept;

    // ── the maintenance thread ─────────────────────────────────────
    // One pass over the region's names, or nothing when the interval has not elapsed. Never throws: a
    // region that will not answer leaves the table as every requester already has it. Holds the naming
    // mutex across the region read, so a command cannot claim a slot this pass is about to judge stale.
    void refresh() noexcept;

    // Gives back every turn whose hold ran out, and clears the slot a holder died inside.
    void dropExpiredTurns(const std::function<bool(std::uint32_t)>& isBusy) noexcept;

    // ── what this manager was handed ───────────────────────────────
    cme::Session* session_;

    // ── the domains this node serves ───────────────────────────────
    struct
    {
        // The shm area the entries below are paired with. Outside naming's reach: each slot in it
        // carries its own robust lock, and this node's requesters take that one.
        protocol::SharedArea_t* area;

        // One mutex for the whole table, not one entry: a refresh judges every entry against one
        // region read, and a claim landing inside that read would have its name retired by it.
        std::mutex naming;

        // Every live name and the id it sits at. The two writers of a name are the only writers here,
        // and both hold the mutex above already.
        protocol::IdLookup ids;

        // By pointer so the definition can stay in the .cpp. An id never moves while live, so neither
        // does the entry it names.
        std::unique_ptr<LocalDomain[]> entries;
    } domains_;

    // ── what this run was configured with ──────────────────────────
    // daemon outlives this manager: main builds it before the region and holds it until the loops have
    // stopped. Its values are read where they are used rather than copied field by field.
    struct
    {
        const DaemonConfig_t* daemon;

        // When the next pass may read the region, from the interval the config names. Unguarded, since
        // the maintenance thread is its only reader and its only writer.
        timing::Deadline refresh{timing::Millis::zero()};
    } config_;

    // ── what a run counted ─────────────────────────────────────────
    // Several workers grant at once, each on its own domain, and these are read from outside this class.
    observe::EventCounts<observe::DomainEvent> observe_;

    // The clock reads that go with them. Always on: one read per acquire is what the path it measures
    // already pays many times over, and a knob would mean the number is absent when it is wanted.
    observe::SpanSums<observe::SpanEvent> spent_;
};

}  // namespace cmed::daemon
