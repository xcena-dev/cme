// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared.cpp -- Session / Guard implementation.

#include "cme/shared.hpp"

#include <chrono>
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

    // Resolve name to Active domain id; throws UnknownDomainError on miss.
    [[nodiscard]] DomainId resolveDomainName(std::string_view name) const
    {
        const DomainId domainId = peer->resolveDomainName(name);
        if (isNoDomain(domainId))
        {
            throw UnknownDomainError{std::string{"cme: unknown domain: "} + std::string{name}};
        }
        return domainId;
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
    auto peerGuard = impl_->peer->lock(impl_->resolveDomainName(name));  // throws LockTimeoutError
    auto guardImpl = std::make_unique<Guard::Impl>();
    guardImpl->peerGuard = std::move(peerGuard);
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
    const DomainId domainId = impl_->resolveDomainName(name);
    auto peerGuardOpt = impl_->peer->tryLock(domainId, timeout);
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
    impl_->peer->joinDomain(impl_->resolveDomainName(name));
}

void Session::leaveDomain(std::string_view name)
{
    if (!impl_)
    {
        throw JoinError{"cme::Session::leaveDomain: session not joined"};
    }
    impl_->peer->leaveDomain(impl_->resolveDomainName(name));
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
    impl_->peer->deleteDomain(impl_->resolveDomainName(name));
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
