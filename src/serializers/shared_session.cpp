// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared_session.cpp -- SharedSession implementation (intra-node tier with cohorting).

#include "cme/shared_session.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "cme/errors.hpp"
#include "cme/shared.hpp"

namespace cme
{

namespace
{

// Default local batch before a forced handoff; mirrors the benchmark harness's cap.
constexpr std::uint32_t DefaultCohortCap = 16;

// Per-domain local tier. Held behind unique_ptr so the address stays valid while a Guard holds
// a unique_lock on `mutex` and the map grows underneath.
struct DomainTier_t
{
    std::mutex mutex;
    // The peer's CXL ownership, kept alive across a whole cohort rather than per critical
    // section. Reset == ~cme::Guard == release, which lets a remote peer in.
    std::optional<cme::Guard> held;
    std::uint32_t batch{0};                 // local acquires since the last handoff
    std::atomic<std::uint32_t> waiters{0};  // threads queued on `mutex`
};

// Release the cohort. Runs with `mutex` still held, so the count it reads cannot change under
// it: keep ownership when another local thread is queued, hand it back when none is.
void releaseCohort(DomainTier_t& tier) noexcept
{
    if (tier.waiters.load(std::memory_order_relaxed) == 0)
    {
        tier.held.reset();
        tier.batch = 0;
    }
}

}  // namespace

struct SharedSession::Impl
{
    explicit Impl(Session sessionIn) noexcept
        : session{std::move(sessionIn)}
    {
    }

    // Lookup only; the map mutex is released before the caller touches the tier.
    [[nodiscard]] DomainTier_t* findTier(std::string_view name)
    {
        const std::lock_guard<std::mutex> guard{mapMutex};
        const auto found = domains.find(name);
        return found == domains.end() ? nullptr : found->second.get();
    }

    void ensureEntry(std::string_view name)
    {
        const std::lock_guard<std::mutex> guard{mapMutex};
        if (domains.find(name) == domains.end())
        {
            domains.emplace(std::string{name}, std::make_unique<DomainTier_t>());
        }
    }

    // Hand the domain back before we stop participating in it: a cohort that still holds
    // ownership would outlive our participation.
    void dropHeldOwnership(std::string_view name)
    {
        DomainTier_t* tier = findTier(name);
        if (tier == nullptr)
        {
            return;
        }
        const std::lock_guard<std::mutex> guard{tier->mutex};
        tier->held.reset();
        tier->batch = 0;
    }

    Session session;
    // Lock order is always mapMutex then DomainTier_t::mutex, and mapMutex is dropped before the
    // tier is locked on the lock() path.
    std::mutex mapMutex;
    // std::less<> for heterogeneous lookup: find(string_view) without allocating a key on
    // every acquire. Domain counts are small, so the tree walk is cheaper than the malloc.
    std::map<std::string, std::unique_ptr<DomainTier_t>, std::less<>> domains;
    std::atomic<std::uint32_t> cohortCap{DefaultCohortCap};
};

struct SharedSession::Guard::Impl
{
    DomainTier_t* tier{nullptr};
    std::unique_lock<std::mutex> entryLock;
};

// ── Guard ───────────────────────────────────────────────────────────

SharedSession::Guard::Guard(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)}
{
}

SharedSession::Guard::Guard(Guard&& other) noexcept = default;

SharedSession::Guard::~Guard() noexcept
{
    // A moved-from Guard owns neither the impl nor the lock, so its release already ran.
    if (impl_ && impl_->entryLock.owns_lock())
    {
        releaseCohort(*impl_->tier);  // with the mutex held; entryLock unlocks after
    }
}

// ── SharedSession ───────────────────────────────────────────────────

SharedSession::SharedSession(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)}
{
}

SharedSession::SharedSession(SharedSession&&) noexcept = default;
SharedSession& SharedSession::operator=(SharedSession&&) noexcept = default;
SharedSession::~SharedSession() = default;

SharedSession SharedSession::open(std::string_view uri)
{
    return SharedSession{std::make_unique<Impl>(Session::open(uri))};
}

SharedSession SharedSession::open(std::string_view uri, const Session::OpenOpts_t& opts)
{
    return SharedSession{std::make_unique<Impl>(Session::open(uri, opts))};
}

SharedSession::Guard SharedSession::lock(std::string_view name)
{
    DomainTier_t* tier = impl_->findTier(name);
    if (tier == nullptr)
    {
        throw NotParticipatingError{std::string{"cme::SharedSession::lock: not joined: "} +
                                    std::string{name}};
    }

    // Announce intent before queuing so the thread currently in the cohort keeps the peer's
    // ownership across the handoff gap instead of releasing it to a remote peer.
    tier->waiters.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> entryLock{tier->mutex};
    tier->waiters.fetch_sub(1, std::memory_order_relaxed);

    if (!tier->held.has_value() || tier->batch >= impl_->cohortCap.load(std::memory_order_relaxed))
    {
        tier->batch = 0;
        tier->held.reset();  // release first: re-acquiring yields to remote peers
        tier->held = impl_->session.lock(name);
    }
    ++tier->batch;

    auto guardImpl = std::make_unique<Guard::Impl>();
    guardImpl->tier = tier;
    guardImpl->entryLock = std::move(entryLock);
    return Guard{std::move(guardImpl)};
}

DomainHandle_t SharedSession::joinDomain(std::string_view name)
{
    const DomainHandle_t joined = impl_->session.joinDomain(name);
    impl_->ensureEntry(name);
    return joined;
}

void SharedSession::leaveDomain(std::string_view name)
{
    impl_->dropHeldOwnership(name);
    impl_->session.leaveDomain(name);
    // The tier is kept, not erased: destroying its mutex while a Guard still holds a
    // unique_lock on it would be undefined. A later rejoin reuses it.
}

DomainHandle_t SharedSession::createDomain(std::string_view name)
{
    const DomainHandle_t created = impl_->session.createDomain(name);
    impl_->ensureEntry(name);  // the creator participates, so it can lock straight away
    return created;
}

void SharedSession::deleteDomain(std::string_view name)
{
    impl_->dropHeldOwnership(name);
    impl_->session.deleteDomain(name);
}

void SharedSession::setCohortCap(std::uint32_t cap) noexcept
{
    impl_->cohortCap.store(cap, std::memory_order_relaxed);
}

}  // namespace cme
