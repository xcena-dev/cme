// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared.hpp -- cme v0.2 user-facing API: name-based mutual exclusion over a shared region.
//
// Two classes: cme::Session (region participant, static factories format/open) and
// cme::Guard (RAII lock holder for one domain, dtor releases). See docs/design/technical_report.md.
//
// THREADING. One Session serves several threads of one process, with one exception:
// lock/tryLock/withLock on the SAME domain do not exclude each other. The ownership token is
// per peer, so a second thread of this peer takes it on the resident fast path and both run the
// critical section. This fails silently -- no error, no timeout. Use cme::SharedSession
// (shared_session.hpp) whenever more than one thread locks a domain.
// Safe concurrently: format, open, join/leave/create/deleteDomain, getDomainNames, and locking
// distinct domains. A Guard is owned by the thread that took it. Move-assigning or destroying a
// Session concurrently with any call on it is not supported.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Exported: every call below throws these, so a caller including this one has to be able to catch
// them without being sent to a second header for the names.
#include "cme/errors.hpp"  // IWYU pragma: export

namespace cme
{

// Domain index, 0..maxDomains-1. The alias core/types.hpp declares, repeated here so a caller can hold
// one without reaching into the internal headers.
using DomainId = std::uint32_t;

// A domain and the incarnation of the slot it was found in. An index alone cannot say which domain it
// names: a slot freed and claimed again keeps its index, so an index kept across that would acquire
// whatever took the slot and report nothing. The incarnation is what refuses instead.
// Zero names no domain: a free slot reads zero, so a default-constructed handle is refused rather than
// matching one.
struct DomainHandle_t
{
    DomainId id;
    std::uint64_t incarnation;
};

// Successor-policy kind. Recorded in region header; joiners use matching impl.
//   Order      -- token-ring, fair under symmetric load
//   Request    -- hand-raise / grant, lower latency under bursts (default)
//   RequestAgg -- Request + packed pendingDomains aggregation for holder scan
//   Peterson   -- tournament Peterson lock, per-domain tree, bounded-wait
enum class Strategy : std::uint8_t
{
    Order = 0,
    Request = 1,
    RequestAgg = 2,
    Peterson = 3,
};

// What makes this peer's writes reach the others, and its reads miss a stale copy.
// A property of how THIS peer reaches the region, not of the region: two peers over one
// device may differ, one mapping devdax WB while the other goes through a marufs UC file.
// So it is set per Session at format/open, and peers of one region need not agree.
//   CacheCoherent -- hardware keeps the peers in sync (shm, or WB with every peer on this host)
//   Uncached      -- no cached copy exists (marufs UC); order and drain, never flush
//   Flush         -- WB cached outside the coherence domain the peers share
// Measured cost of picking wrong (64B op, this bench): flushing a coherent WB read is
// 0.5 ns -> 327 ns, and flushing a UC read adds 94 ns for nothing.
enum class CoherencyMode : std::uint8_t
{
    CacheCoherent = 0,
    Uncached = 1,
    Flush = 2,
};

// RAII lock holder. Constructed via Session::lock / tryLock / withLock.
// Holds the ME token for one domain until unlock() or destruction.
class [[nodiscard]] Guard
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // An empty Guard, for a caller that locks conditionally and moves the result in later.
    // Declared here and defined in shared.cpp: `= default` in the header is generated in the
    // caller's translation unit, where Impl is incomplete, so it would not compile there.
    Guard() noexcept;
    Guard(const Guard&) = delete;
    Guard(Guard&& other) noexcept;
    ~Guard() noexcept;

    // ── operator= ──────────────────────────────────────────────────
    Guard& operator=(const Guard&) = delete;
    Guard& operator=(Guard&& other) noexcept;

    // ── public methods ─────────────────────────────────────────────
    // Explicit early release. Idempotent.
    void unlock() noexcept;

    // Whether this Guard holds a domain. False for an empty one, and false after it has been
    // moved from: a move leaves the source holding nothing.
    explicit operator bool() const noexcept
    {
        return impl_ != nullptr;
    }

private:
    friend class Session;
    struct Impl;
    explicit Guard(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

class Session
{
public:
    // ── nested types ───────────────────────────────────────────────
    struct FormatOpts_t
    {
        // Slot ceiling including control domain (slot 0); data domains in slots 1..maxDomains-1.
        std::uint32_t maxDomains{8};
        std::uint32_t maxPeers{8};
        Strategy strategy{Strategy::Request};
        // RequestAgg only: number of aggregator groups (peer p -> group p % groups).
        // 0 = auto (minimum that keeps one packed line per group). Ignored otherwise.
        std::uint32_t aggregatorGroups{0};
    };

    struct OpenOpts_t
    {
        std::chrono::milliseconds formatTimeout{5'000};  // wait for creator to finish format
        CoherencyMode coherency{CoherencyMode::CacheCoherent};
    };

    // ── ctor / dtor ────────────────────────────────────────────────
    Session(const Session&) = delete;
    Session(Session&&) noexcept;
    ~Session();  // leave + slot release

    // ── operator= ──────────────────────────────────────────────────
    Session& operator=(const Session&) = delete;
    Session& operator=(Session&&) noexcept;

    // ── factories ──────────────────────────────────────────────────
    // Zeroes and re-initialises the region: mkfs, not create-if-absent. Calling it on a
    // live region discards what its peers are using, so only the process setting the region
    // up calls it. A joiner's open() waits OpenOpts_t::formatTimeout for a format in flight;
    // two concurrent formatters are not serialised against each other. The URI scheme selects
    // the backend: "dax:", "shm:", "file:". Throws FormatError on backend/size issues.
    static void format(std::string_view uri, const FormatOpts_t& opts);

    // Attach + claim a peer slot. Throws RegionNotFormattedError, NoFreeSlotError, JoinError.
    [[nodiscard]] static Session open(std::string_view uri);
    [[nodiscard]] static Session open(std::string_view uri, const OpenOpts_t& opts);

    // ── ops ────────────────────────────────────────────────────────
    // Blocking acquire, bounded by AcquireTimeout. Throws LockTimeoutError / UnknownDomainError.
    // Excludes other peers, not other threads of this one -- see THREADING above.
    [[nodiscard]] Guard lock(std::string_view name);

    // Bounded acquire. Returns nullopt on deadline only; throws UnknownDomainError
    // on unknown/deleted name (so a retry loop on a bad name fails fast, not spins).
    [[nodiscard]] std::optional<Guard>
    tryLock(std::string_view name,
            std::chrono::nanoseconds timeout = std::chrono::seconds{5});

    // Scoped lambda helper. fn receives a reference to the Guard.
    template <typename T>
    void withLock(std::string_view name, T&& body)
    {
        auto guard = lock(name);
        std::forward<T>(body)(guard);
    }

    // ── participation (opt-in) ─────────────────────────────────────
    // lock() on a non-joined domain throws NotParticipatingError.
    // joinDomain is idempotent. Unknown name -> UnknownDomainError.
    void joinDomain(std::string_view name);
    void leaveDomain(std::string_view name);

    // ── by handle ───────────────────────────────────────────────────
    // The handle a name resolves to. Resolving by name walks the region's domain records, so a caller
    // that locks the same domain repeatedly pays that walk once here. Throws UnknownDomainError.
    [[nodiscard]] DomainHandle_t resolveDomain(std::string_view name) const;

    // Acquire by handle, without the walk. A handle whose domain was deleted throws UnknownDomainError
    // rather than acquiring the domain that took its slot.
    //
    // The incarnation is compared once before the turn and once with it held. Only the second decides:
    // deleting a domain takes its lock, so nothing can replace the domain under a granted turn.
    [[nodiscard]] Guard lock(DomainHandle_t handle);

    // The bounded form of the same, so a caller that resolved once has both shapes by handle. nullopt is
    // the timeout alone; a handle that names no live domain throws, as it does through lock.
    [[nodiscard]] std::optional<Guard>
    tryLock(DomainHandle_t handle, std::chrono::nanoseconds timeout = std::chrono::seconds{5});

    // ── dynamic domains (create / delete) ───────────────────────────
    // Serialised via control domain. Throws DomainExistsError / DomainLimitError.
    void createDomain(std::string_view name);
    // Caller must be sole participant. Throws UnknownDomainError / NotParticipatingError.
    void deleteDomain(std::string_view name);

    // ── accessors ──────────────────────────────────────────────────
    // Names of all live (Active) data domains; excludes the control domain.
    [[nodiscard]] std::vector<std::string> getDomainNames() const;

private:
    struct Impl;
    explicit Session(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

// Publish caller-owned dirty cachelines to FAM. Call inside a Guard scope after writing your
// own data; cme flushes its metadata itself. Pass the same CoherencyMode the Session was
// opened with -- Flush is the only value that emits a flush, the others just fence.
void flush(const void* addr, std::size_t bytes, CoherencyMode mode) noexcept;

}  // namespace cme
