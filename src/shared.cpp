// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared.cpp -- Session / Guard implementation.

#include "cme/shared.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "admission/claim.hpp"
#include "cme/errors.hpp"
#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "util/coherency.hpp"

namespace cme
{

namespace
{

Geometry::FormatOpts_t makeInternalFormatOpts(const Session::FormatOpts_t& opts)
{
    // maxDomains includes control(0); need >= 2 for at least one data domain.
    if (opts.maxDomains < 2 || opts.maxDomains > MaxDomains)
    {
        throw FormatError{"cme: maxDomains out of range"};
    }
    if (opts.maxPeers == 0 || opts.maxPeers > MaxPeers)
    {
        throw FormatError{"cme: maxPeers out of range"};
    }
    return Geometry::FormatOpts_t{
        opts.strategy,
        opts.aggregatorGroups,
    };
}

}  // namespace

// ── Guard::Impl ───────────────────────────────────────────────────
struct Guard::Impl
{
    PeerGuard peerGuard;
};

Guard::Guard(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)}
{
}

// Out of line, all four: Impl is complete only here, and a header-side `= default` would be
// generated in the caller's translation unit where it is not.
Guard::Guard() noexcept = default;
Guard::Guard(Guard&&) noexcept = default;
Guard& Guard::operator=(Guard&&) noexcept = default;
Guard::~Guard() noexcept = default;

void Guard::unlock() noexcept
{
    if (impl_)
    {
        impl_->peerGuard.release();
        impl_.reset();
    }
}

// ── Session::Impl ─────────────────────────────────────────────────
struct Session::Impl
{
    // By value, not optional: a Session that exists has a region. open() is the only way to
    // make one, and it cannot get past Geometry::open without one.
    explicit Impl(Geometry region) noexcept
        : geometry{std::move(region)}
    {
    }

    Geometry geometry;
    std::unique_ptr<Peer> peer;

    // Resolve name to Active domain id and the incarnation that slot carried; UnknownDomainError on miss.
    // One walk answers both words. Read by two calls, a delete and create between them pair the slot
    // with the incarnation of whatever took it, and that pair names a domain nobody resolved.
    [[nodiscard]] DomainHandle_t resolveHandle(std::string_view name) const
    {
        std::uint64_t incarnation = 0;
        const DomainId domainId = peer->resolveDomainName(name, incarnation);
        if (isNoDomain(domainId))
        {
            throw UnknownDomainError{std::string{"cme: unknown domain: "} + std::string{name}};
        }
        return DomainHandle_t{domainId, incarnation};
    }

    // A handle naming a slot that now holds another domain would otherwise acquire that one and answer
    // as if it were the domain the caller resolved.
    //
    // Zero is refused without asking the region. A free slot reads 0, so a handle carrying 0 compares
    // equal to a slot holding no domain at all.
    void refuseStaleHandle(DomainHandle_t handle) const
    {
        if (handle.incarnation == 0 || peer->readDomainIncarnation(handle.id) != handle.incarnation)
        {
            throw UnknownDomainError{"cme: the domain this handle named is gone"};
        }
    }

    // Called once before the acquire and once with the turn held. The first refusal saves a turn nobody
    // wants; only the second is final, because deleting a domain takes its lock and so cannot run
    // between the check and the grant once that lock is ours.
    [[nodiscard]] PeerGuard lockByHandle(DomainHandle_t handle) const
    {
        refuseStaleHandle(handle);
        auto peerGuard = peer->lock(handle.id);  // throws LockTimeoutError
        refuseStaleHandle(handle);
        return peerGuard;
    }

    // The same pair around a tryLock. A timeout stays nullopt; a handle whose domain went is refused
    // with the error lock throws, because the caller asked for a domain and there is none to wait for.
    [[nodiscard]] std::optional<PeerGuard> tryLockByHandle(DomainHandle_t handle, timing::Nanos timeout) const
    {
        refuseStaleHandle(handle);
        auto peerGuardOpt = peer->tryLock(handle.id, timeout);
        if (!peerGuardOpt)
        {
            return std::nullopt;
        }
        refuseStaleHandle(handle);
        return peerGuardOpt;
    }
};

Session::Session(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)}
{
}

Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
Session::~Session() = default;

void Session::format(std::string_view uri, const FormatOpts_t& opts)
{
    const auto internalOpts = makeInternalFormatOpts(opts);
    const auto domainCount = static_cast<DomainId>(opts.maxDomains);
    const auto peerCount = opts.maxPeers;

    static_cast<void>(Geometry::create(uri, domainCount, peerCount, internalOpts));
}

Session Session::open(std::string_view uri)
{
    return open(uri, OpenOpts_t{});
}

Session Session::open(std::string_view uri, const OpenOpts_t& opts)
{
    auto geometry = Geometry::open(uri);
    geometry.bindBlocking(opts.formatTimeout, opts.coherency);

    const auto peerId = admission::claimPeerSlot(geometry, opts.coherency);

    auto impl = std::make_unique<Session::Impl>(std::move(geometry));
    impl->peer = std::make_unique<Peer>(impl->geometry, peerId, opts.coherency);
    return Session{std::move(impl)};
}

Guard Session::lock(std::string_view name)
{
    if (!impl_)
    {
        throw JoinError{"cme::Session::lock: session not joined"};
    }
    // Through a handle, not straight to the id the walk found. A name resolved to a bare id has the same
    // window the handle exists to close: the slot can change hands before the turn is granted.
    auto peerGuard = impl_->lockByHandle(impl_->resolveHandle(name));  // throws LockTimeoutError
    auto guardImpl = std::make_unique<Guard::Impl>();
    guardImpl->peerGuard = std::move(peerGuard);
    return Guard{std::move(guardImpl)};
}

DomainHandle_t Session::resolveDomain(std::string_view name) const
{
    if (!impl_)
    {
        throw JoinError{"cme::Session::resolveDomain: session not joined"};
    }

    return impl_->resolveHandle(name);
}

Guard Session::lock(DomainHandle_t handle)
{
    if (!impl_)
    {
        throw JoinError{"cme::Session::lock: session not joined"};
    }
    auto peerGuard = impl_->lockByHandle(handle);
    auto guardImpl = std::make_unique<Guard::Impl>();
    guardImpl->peerGuard = std::move(peerGuard);
    return Guard{std::move(guardImpl)};
}

std::optional<Guard> Session::tryLock(DomainHandle_t handle, timing::Nanos timeout)
{
    if (!impl_)
    {
        return std::nullopt;
    }
    auto peerGuardOpt = impl_->tryLockByHandle(handle, timeout);
    if (!peerGuardOpt)
    {
        return std::nullopt;
    }
    auto guardImpl = std::make_unique<Guard::Impl>();
    guardImpl->peerGuard = std::move(*peerGuardOpt);
    return Guard{std::move(guardImpl)};
}

std::optional<Guard> Session::tryLock(std::string_view name,
                                      timing::Nanos timeout)
{
    if (!impl_)
    {
        return std::nullopt;
    }
    // Unknown/deleted name throws (same as lock()); nullopt is timeout-only.
    auto peerGuardOpt = impl_->tryLockByHandle(impl_->resolveHandle(name), timeout);
    if (!peerGuardOpt)
    {
        return std::nullopt;
    }
    auto guardImpl = std::make_unique<Guard::Impl>();
    guardImpl->peerGuard = std::move(*peerGuardOpt);
    return Guard{std::move(guardImpl)};
}

void Session::joinDomain(std::string_view name)
{
    if (!impl_)
    {
        throw JoinError{"cme::Session::joinDomain: session not joined"};
    }
    const DomainHandle_t handle = impl_->resolveHandle(name);
    impl_->peer->joinDomain(handle.id, handle.incarnation);
}

void Session::leaveDomain(std::string_view name)
{
    if (!impl_)
    {
        throw JoinError{"cme::Session::leaveDomain: session not joined"};
    }
    const DomainHandle_t handle = impl_->resolveHandle(name);
    impl_->peer->leaveDomain(handle.id, handle.incarnation);
}

void Session::createDomain(std::string_view name)
{
    if (!impl_)
    {
        throw JoinError{"cme::Session::createDomain: session not joined"};
    }
    static_cast<void>(impl_->peer->createDomain(name));
}

void Session::deleteDomain(std::string_view name)
{
    if (!impl_)
    {
        throw JoinError{"cme::Session::deleteDomain: session not joined"};
    }
    const DomainHandle_t handle = impl_->resolveHandle(name);
    impl_->peer->deleteDomain(handle.id, handle.incarnation);
}

std::vector<std::string> Session::getDomainNames() const
{
    std::vector<std::string> names;
    if (!impl_)
    {
        return names;
    }
    // Scan Active data slots (skip control slot 0).
    const std::uint32_t numDomains = impl_->geometry.getDomainCount();
    for (DomainId domainId = 1; domainId < numDomains; ++domainId)
    {
        const auto record = coherency::get(impl_->geometry.getDomainRecord(domainId),
                                           impl_->peer->getCoherencyMode());  // rmb + 64B read
        if (record.isValidMagic() &&
            record.hasState(Geometry::DomainRecord_t::State::Active))
        {
            names.emplace_back(record.getName());
        }
    }
    return names;
}

// ── flush ─────────────────────────────────────────────────────────
void flush(const void* addr, std::size_t bytes, CoherencyMode mode) noexcept
{
    if (addr == nullptr || bytes == 0)
    {
        return;
    }
    coherency::wmb(addr, bytes, mode);
}

}  // namespace cme
