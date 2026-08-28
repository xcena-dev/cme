// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/domain/local_domain.hpp -- what this daemon holds for one domain, and the lock over it.
//
// The third layer of one domain: the region's record is the truth, the shm slot is this node's
// projection, and this is what the daemon alone knows. Guard is the only way in.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::daemon
{

// One domain's worth of what this node holds: the turn, how long it may keep it, the handle it was
// taken on, and how many requesters asked to stay. Reached only through Guard, which locks it.
class LocalDomain
{
public:
    // The mutex and the way in, together: every member below is private, so a caller reaches one only
    // by holding this. Built on a reference, since an id with no entry is refused before it gets here.
    class [[nodiscard]] Guard
    {
    public:
        explicit Guard(LocalDomain& entry) noexcept
            : holding_{entry.counting_},
              entry_{entry}
        {
            entry_.noteLocked();
        }

        Guard(const Guard&) = delete;
        Guard(Guard&&) = delete;

        ~Guard() noexcept
        {
            entry_.noteUnlocked();
        }

        Guard& operator=(const Guard&) = delete;
        Guard& operator=(Guard&&) = delete;

        [[nodiscard]] LocalDomain* operator->() const noexcept
        {
            return &entry_;
        }

    private:
        std::unique_lock<std::mutex> holding_;
        LocalDomain& entry_;
    };

    // Paired once at startup, since neither half of a domain ever moves. Outside counting_'s reach:
    // the shm half has its own robust lock, and a requester takes that one without seeing this mutex.
    void pairSlot(std::uint32_t domainId, protocol::Domain_t& half) noexcept
    {
        domainId_ = domainId;
        slot_ = &half;
    }

    [[nodiscard]] protocol::Domain_t& slot() const noexcept
    {
        return *slot_;
    }

    // The number a requester and this node's workers know this domain by. Kept because those two
    // speak in ids, not because anything inside here needs one.
    [[nodiscard]] std::uint32_t readId() const noexcept
    {
        return domainId_;
    }

    // ── the turn ───────────────────────────────────────────────────
    [[nodiscard]] bool hasTurn() const noexcept
    {
        assertLocked();
        return guard_.has_value();
    }

    // The three together, because a turn without the handle it was taken on cannot be given back to
    // the right domain, and one without a deadline would be held until the daemon stops.
    void takeTurn(cme::Guard taken, cme::DomainHandle_t handle, timing::Millis hold) noexcept
    {
        assertLocked();
        guard_ = std::move(taken);
        resolved_ = handle;
        runUntil_ = timing::Deadline{hold};
    }

    // Destroying the Guard is the hand-back. Doing it on a hand-back rather than at every release is
    // the whole of cohorting: the next local requester finds the turn already taken.
    void releaseTurn() noexcept
    {
        assertLocked();
        guard_.reset();
    }

    // What the join answered, and what the acquire was made on. The incarnation in it refuses a slot
    // that changed hands, so a leave by this handle leaves the domain the turn was taken for.
    [[nodiscard]] cme::DomainHandle_t turnHandle() const noexcept
    {
        assertLocked();
        return resolved_;
    }

    // Whether this run may keep the turn no longer, so a busy node cannot hold a domain against every
    // other host for as long as it stays busy.
    [[nodiscard]] bool runExpired() const noexcept
    {
        assertLocked();
        return runUntil_.expired();
    }

    // ── the requesters counted here ────────────────────────────────
    void addJoin() noexcept
    {
        assertLocked();
        ++joined_;
    }

    // False when there was none to drop, which is a caller leaving what it never joined rather
    // than a failure. The count would wrap, and a wrapped count never reaches zero again.
    [[nodiscard]] bool dropJoin() noexcept
    {
        assertLocked();
        if (joined_ == 0)
        {
            return false;
        }
        --joined_;
        return true;
    }

    // The delete retracted this peer's participation on its way out, so the count is all the entry
    // has left to clear. Apart from dropJoin, which answers one requester and not the domain.
    void clearJoins() noexcept
    {
        assertLocked();
        joined_ = 0;
    }

    // Whether anything here still needs this node inside the domain, by a requester's join or by
    // the turn it holds. A delete asks first, because cme refuses one from a peer that is outside.
    [[nodiscard]] bool keepsParticipation() const noexcept
    {
        assertLocked();
        return joined_ > 0 || guard_.has_value();
    }

private:
    // Per entry and not per table: a worker granting one domain has nothing to say about the counts
    // of another, and one mutex for the table made it wait anyway.
    std::mutex counting_;

    std::optional<cme::Guard> guard_;
    timing::Deadline runUntil_{timing::Millis::zero()};
    cme::DomainHandle_t resolved_{0, 0};
    std::uint32_t joined_{0};
    std::uint32_t domainId_{0};
    protocol::Domain_t* slot_{nullptr};

    // Whose turn it is to touch the fields above. A holder check and not an "is it locked" one: a
    // mutex another thread holds reads as locked, which is the case worth catching.
    std::thread::id holder_;

    void noteLocked() noexcept
    {
        holder_ = std::this_thread::get_id();
    }

    void noteUnlocked() noexcept
    {
        holder_ = std::thread::id{};
    }

    // Ends the run rather than carrying on. Reaching here means the participation count is already
    // racing, and a wrong count leaves the node inside a domain no other node may then delete.
    void assertLocked() const noexcept
    {
        if (holder_ != std::this_thread::get_id())
        {
            std::abort();
        }
    }
};

}  // namespace cmed::daemon
