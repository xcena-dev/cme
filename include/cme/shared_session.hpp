// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared_session.hpp -- SharedSession: many threads of one process over one Session.
//
// cme's ownership token is per PEER, not per thread: a second thread of this peer passes the
// domain lock immediately on the resident fast path, so plain Session::lock does not serialise
// this process's own threads. SharedSession adds the missing intra-node tier -- a per-domain
// std::mutex over the inter-node CXL ownership -- and cohorts it: ownership is acquired once and
// reused by a batch of local waiters before being handed back, capped so remote peers cannot
// starve. Use this whenever more than one thread locks the same domain.

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "cme/shared.hpp"

namespace cme
{

class SharedSession
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    SharedSession(const SharedSession&) = delete;
    SharedSession(SharedSession&&) noexcept;
    ~SharedSession();

    // ── operator= ──────────────────────────────────────────────────
    SharedSession& operator=(const SharedSession&) = delete;
    SharedSession& operator=(SharedSession&&) noexcept;

    // ── factories ──────────────────────────────────────────────────
    // Owns the wrapped Session: handing out the raw Session would let a caller bypass the
    // intra-node tier, which is the whole point of this class.
    [[nodiscard]] static SharedSession open(std::string_view uri);
    [[nodiscard]] static SharedSession open(std::string_view uri, const Session::OpenOpts_t& opts);

    // ── ops ────────────────────────────────────────────────────────
    // RAII holder for one domain: holds this process's per-domain mutex, and keeps the peer's
    // CXL ownership alive for the next local waiter (cohorting) rather than releasing it.
    class [[nodiscard]] Guard
    {
    public:
        // ── ctor / dtor ────────────────────────────────────────────
        // No default constructor, unlike cme::Guard: move-assign is deleted below, so an empty
        // Guard could never become a held one. lock() is the only way to get one.
        Guard(const Guard&) = delete;
        Guard(Guard&& other) noexcept;
        ~Guard() noexcept;

        // ── operator= ──────────────────────────────────────────────
        Guard& operator=(const Guard&) = delete;
        // Deleted, not defaulted: it would drop the mutex without running the cohort release,
        // leaving the domain pinned and unforwardable forever.
        Guard& operator=(Guard&&) = delete;

        // Whether this Guard holds the domain. False after it has been moved from: a move leaves
        // the source holding nothing.
        explicit operator bool() const noexcept
        {
            return impl_ != nullptr;
        }

    private:
        friend class SharedSession;
        struct Impl;
        explicit Guard(std::unique_ptr<Impl> impl) noexcept;

    private:
        std::unique_ptr<Impl> impl_;
    };

    // Blocking acquire. Throws the same errors as Session::lock, plus NotParticipatingError if
    // the name was never joined through this object. The local mutex is held for the Guard's
    // whole lifetime, and Session::lock runs under it, so a queued thread waits out the
    // inter-node acquire too -- there is no separate local deadline.
    [[nodiscard]] Guard lock(std::string_view name);

    // ── participation ──────────────────────────────────────────────
    // Join before locking, as with Session. Not safe to call concurrently with lock() on the
    // same name: it is the call that creates that domain's local mutex.
    void joinDomain(std::string_view name);

    // PRECONDITION for leaveDomain and deleteDomain: this thread holds no Guard for @name.
    // Both take that domain's local mutex to hand ownership back first, and a Guard holds the
    // same mutex for its whole lifetime -- calling either inside a locked scope self-deadlocks
    // on a non-recursive std::mutex. Let the Guard go out of scope first.
    void leaveDomain(std::string_view name);

    // ── dynamic domains ────────────────────────────────────────────
    void createDomain(std::string_view name);
    void deleteDomain(std::string_view name);

    // ── tuning ─────────────────────────────────────────────────────
    // Max consecutive local acquisitions that reuse the peer's held CXL ownership before a
    // forced handoff. Bounds how long remote peers wait. 1 = no cohorting (every critical
    // section does a full acquire/release).
    void setCohortCap(std::uint32_t cap) noexcept;

private:
    struct Impl;
    explicit SharedSession(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace cme
