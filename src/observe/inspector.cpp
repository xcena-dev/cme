// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// inspector.cpp -- read-only region observer (rebinds before each sample).

#include "observe/inspector.hpp"

// strnlen is POSIX, which <cstring> does not declare.
#include <string.h>  // NOLINT(modernize-deprecated-headers)

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

#include "cme/shared.hpp"
#include "core/layout/geometry.hpp"
#include "core/layout/geometry_profile.hpp"
#include "core/policy/request_demand_region.hpp"
#include "core/types.hpp"
#include "util/coherency.hpp"

namespace cme
{

namespace
{

[[nodiscard]] bool isValidDomainId(const Geometry& geometry, DomainId domainId) noexcept
{
    return domainId < geometry.getDomainCount();
}

[[nodiscard]] bool isValidPeerId(const Geometry& geometry, PeerId peerId) noexcept
{
    // NoPeer (UINT32_MAX) is excluded by the unsigned compare.
    return peerId < geometry.getPeerCount();
}

}  // namespace

Inspector::Inspector(Geometry geometry, CoherencyMode coherency) noexcept
    : geometry_{std::move(geometry)},
      coherency_{coherency}
{
}

Inspector Inspector::open(std::string_view uri, CoherencyMode coherency)
{
    return Inspector{Geometry::open(uri), coherency};
}

std::optional<Inspector::HeaderSnapshot_t> Inspector::readHeader() const
{
    if (!geometry_.rebind({}, coherency_))
    {
        return std::nullopt;
    }
    auto* header = geometry_.getHeader();
    HeaderSnapshot_t snapshot{};
    snapshot.strategy = header->getStrategy();
    snapshot.numDomains = geometry_.getDomainCount();
    snapshot.maxPeers = geometry_.getPeerCount();
    snapshot.formatGeneration = header->formatGeneration;
    snapshot.totalSize = static_cast<std::uint64_t>(header->totalSize);
    return snapshot;
}

std::optional<Inspector::OwnershipSnapshot_t> Inspector::readOwnership(DomainId domainId) const
{
    if (!geometry_.rebind({}, coherency_) || !isValidDomainId(geometry_, domainId))
    {
        return std::nullopt;
    }
    const auto domainRecord = coherency::get(geometry_.getDomainRecord(domainId), coherency_);  // rmb + 64B read
    if (!domainRecord.isValidMagic())
    {
        return std::nullopt;
    }
    OwnershipSnapshot_t snapshot{};
    snapshot.holder = domainRecord.holder;
    snapshot.epoch = domainRecord.epoch;
    // Bounded read: the record is a fixed array, and a torn or never-written name has no
    // terminator to trust.
    snapshot.name.assign(domainRecord.name,
                         ::strnlen(domainRecord.name, Geometry::DomainRecord_t::MaxNameLen));
    return snapshot;
}

std::optional<Inspector::PeerSnapshot_t> Inspector::readPeer(PeerId peerId) const
{
    if (!geometry_.rebind({}, coherency_) || !isValidPeerId(geometry_, peerId))
    {
        return std::nullopt;
    }
    const std::uint32_t peerIndex = peerId;

    const auto memberSlot = coherency::get(geometry_.getMemberSlot(peerIndex), coherency_);  // rmb + 64B read
    if (!memberSlot.isValidMagic())
    {
        return std::nullopt;
    }

    PeerSnapshot_t snapshot{};
    snapshot.active = memberSlot.hasStatus(Geometry::Member_t::Status::Active);
    snapshot.selfId = peerId;
    snapshot.lastSeenNanos = memberSlot.lastSeenNanos;
    // REQUEST demand is policy-private (successor demand region), not in the member slot.
    const Strategy strategy = geometry_.getStrategy();
    snapshot.pendingDomains = (strategy == Strategy::Request || strategy == Strategy::RequestAgg)
                                  ? request_demand::loadPending(geometry_.getSuccessorAreaBase(), peerId, coherency_)
                                  : DomainBitmap{};

    // readProfile gates build toggle + null + magic, and resolveProfileSlot rmb's the
    // slot itself -- no separate barrier here. All-zero when the section is absent.
    snapshot.time = readProfile(geometry_.getProfileSlot(peerIndex), coherency_);
    return snapshot;
}

}  // namespace cme
