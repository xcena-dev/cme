// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cmed/guard.hpp -- RAII holder of one domain's turn. Its own header because the state is held by
// value: a turn is taken and given back per critical section, so a guard behind a pointer would put
// one allocation on every acquire, on a path whose fast half makes no syscall at all.

#pragma once

#include <atomic>
#include <cstdint>

#include "cmed/robust_lock.hpp"

namespace cmed
{

// Named and not included: this holds a pointer into the area and reads it only in the .cpp, so a
// requester's build does not take the whole layout to hold a guard.
namespace protocol
{
struct SharedArea_t;
}

class CmedSession;

// Giving the turn back costs no round trip: the destructor publishes Idle, drops the domain mutex,
// and rings the daemon only when nobody local is queued. An early return or a throw still frees it.
class [[nodiscard]] CmedGuard
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // Only the session builds one. A caller that locks conditionally holds the std::optional
    // tryLock already answers with, rather than a second way to say it has nothing.
    CmedGuard(const CmedGuard&) = delete;
    CmedGuard(CmedGuard&& other) noexcept;
    ~CmedGuard() noexcept;

    // ── operator= ──────────────────────────────────────────────────
    CmedGuard& operator=(const CmedGuard&) = delete;
    CmedGuard& operator=(CmedGuard&& other) noexcept;

    // ── public methods ─────────────────────────────────────────────
    // Explicit early release, idempotent. Nothing on the way out can fail or block, so this path
    // and the destructor's are the same one.
    void unlock() noexcept;

    // ── accessors ──────────────────────────────────────────────────
    // The previous holder died inside its critical section, so what it guarded may be half
    // written. The turn itself is sound; false on a guard holding nothing.
    [[nodiscard]] bool wasAbandoned() const noexcept;

    // The same fact kept in the area, so it survives the acquire that first saw it. Nonzero until a
    // caller that repaired what a dead holder guarded clears it.
    [[nodiscard]] std::uint16_t readAbandonCount() const noexcept;

    // Says what this domain guards is sound again, against the count the caller read. False when a
    // further death landed since, which leaves the repair incomplete rather than clearing it.
    [[nodiscard]] bool clearAbandonCount(std::uint16_t seen) noexcept;

    // Whether the turn's lease still stands. The daemon stamps an expiry when it takes the turn, so a
    // caller past that stamp is outside the turn it thinks it has.
    [[nodiscard]] bool stillHolds() const noexcept;

    explicit operator bool() const noexcept
    {
        return area_ != nullptr;
    }

private:
    // ── ctor / dtor ────────────────────────────────────────────────
    // Every field is decided by the session under the domain mutex, so a guard makes no judgement of
    // its own past this point.
    friend class CmedSession;
    CmedGuard(protocol::SharedArea_t& area, std::atomic<std::uint32_t>& holders, std::uint32_t domainId,
              util::RobustLock held) noexcept;

    // ── giving the turn back ───────────────────────────────────────
    // Publishes Idle and leaves. A requester queued behind the mutex reads that state before it
    // judges the turn, so the daemon writing it from its own pass would race that read.
    void release() noexcept;

private:
    // Null is the whole of holding nothing: a default-built guard, a moved-from one, and one already
    // unlocked all read the same here, so no second flag can disagree with it.
    protocol::SharedArea_t* area_{nullptr};

    // The session's count of guards out of that area. Counted back down on the way out, since a
    // session replacing the area waits for this guard before it unmaps what the line above names.
    std::atomic<std::uint32_t>* holders_{nullptr};

    util::RobustLock held_;

    std::uint32_t domainId_{0};
};

}  // namespace cmed
