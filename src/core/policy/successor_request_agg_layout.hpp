// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor_request_agg_layout.hpp -- what a RequestAgg successor area looks like.
//
// Area = DemandLine_t[peerCount] ++ AggregatorRecord_t[G] ++ AggregatedRequest_t[G]. The
// leading demand region is shared with RequestPolicy; the rest packs over that same demand.
//
// Built once from whatever already knows the dimensions -- a Geometry for an observer, a
// LocalPeerState for the policy -- and every answer after that comes from what it worked out
// at construction. So no accessor takes a width, and none can be handed one the area was
// never formatted at.
//
// That is also the line between this file and the policy: work out where a record sits here,
// decide what to write into it there. Split out because three parties ask the same questions
// and used to answer them apart -- format() lays the records down, bind() finds them again,
// and a test watching a re-election land has to reach the same bytes.
//
// Internal: not in include/cme. Writing an aggregator record can strand a group in
// holder-fallback, so the public API offers operations rather than storage.

#pragma once

#include <cstdint>

#include "core/layout/geometry.hpp"
#include "core/policy/request_demand_region.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "util/coherency.hpp"
#include "util/endian.hpp"
#include "util/util.hpp"

namespace cme
{

// Befriended below: the one caller that formats the area, and so the one with no bound
// object to take the dimensions from.
class RequestAggPolicy;

class RequestAggLayout
{
public:
    // ── records ────────────────────────────────────────────────────────
    // Read and written through coherency::get/set, never field by field: a partial write
    // would leave a group-mate's slice of the same line stale.

    struct AggregatedRequest_t
    {
        // member m (slot-local): packedBitmaps[m*bitmapBytes + d/8] bit d%8
        std::uint8_t packedBitmaps[64];
    };

    struct AggregatorRecord_t
    {
        static constexpr std::uint32_t Magic = 0x43414747u;  // "CAGG"

        endian::Field_t<std::uint32_t> magic;             //  0
        endian::Field_t<std::uint32_t> aggregatorPeerId;  //  4: packs this group; NoPeer = none
        std::uint8_t reserved[56];                        //  8..63: pad to 64 B

        // A record format() never wrote reads as zeroes, which would otherwise pass for
        // "peer 0 is the aggregator". The magic tells a formatted record from a blank one.
        [[nodiscard]] bool isValidMagic() const noexcept
        {
            return static_cast<std::uint32_t>(magic) == Magic;
        }
    };

    // ── sizing, before an area exists ──────────────────────────────────
    // format() and getRegionAreaSize() ask these with dimensions alone.

    // One peer's wanted-domain bitmap width: ceil(domainCount/8).
    [[nodiscard]] static constexpr std::uint32_t getBitmapBytes(std::uint32_t domainCount) noexcept
    {
        return ceilDiv(domainCount, 8u);
    }

    // The requested group count, floored to the minimum that keeps one slot per group
    // (0 = auto -> that minimum).
    [[nodiscard]] static constexpr std::uint32_t getGroupCount(std::uint32_t aggregatorGroups,
                                                               std::uint32_t domainCount,
                                                               std::uint32_t peerCount) noexcept
    {
        const std::uint32_t minGroups = ceilDiv(peerCount, getMembersPerSlot(domainCount));
        return (aggregatorGroups < minGroups) ? minGroups : aggregatorGroups;
    }

    [[nodiscard]] static std::uint64_t getAreaBytes(std::uint32_t domainCount,
                                                    std::uint32_t peerCount,
                                                    std::uint32_t aggregatorGroups) noexcept
    {
        const std::uint32_t groups = getGroupCount(aggregatorGroups, domainCount, peerCount);
        return request_demand::getRegionBytes(peerCount) +
               static_cast<std::uint64_t>(groups) *
                   (sizeof(AggregatorRecord_t) + sizeof(AggregatedRequest_t));
    }

    // ── binding to an area ─────────────────────────────────────────────
    // Both take the dimensions off an object that already carries them, so a caller never
    // states a width of its own. The form that does state one is private.

    // For an observer, which holds the region itself.
    explicit RequestAggLayout(const Geometry& region) noexcept
        : RequestAggLayout{region.getSuccessorAreaBase(), region.getDomainCount(),
                           region.getPeerCount(), region.getHeader()->getAggregatorGroups()}
    {
    }

    // For the policy, whose peer already carries the dimensions it was bound at.
    explicit RequestAggLayout(LocalPeerState& peerState) noexcept
        : RequestAggLayout{peerState.getSuccessorAreaBase(), peerState.getNumDomains(),
                           peerState.getMaxPeers(), peerState.getAggregatorGroups()}
    {
    }

    // ── what it worked out ─────────────────────────────────────────────

    [[nodiscard]] std::uint32_t getGroups() const noexcept
    {
        return groups_;
    }

    [[nodiscard]] std::uint32_t getBitmapBytes() const noexcept
    {
        return bitmapBytes_;
    }

    [[nodiscard]] AggregatorRecord_t* getRecord(std::uint32_t groupId) const noexcept
    {
        return records_ + groupId;
    }

    [[nodiscard]] AggregatedRequest_t* getSlot(std::uint32_t groupId) const noexcept
    {
        return slots_ + groupId;
    }

private:
    // For format(), which is handed the dimensions because the region it is laying down
    // cannot be asked yet. Private because the three counts are all uint32_t: a swapped pair
    // compiles and yields an area whose records sit where nothing else looks for them.
    friend class RequestAggPolicy;
    RequestAggLayout(std::uint8_t* successorAreaBase, std::uint32_t domainCount,
                     std::uint32_t peerCount, std::uint32_t aggregatorGroups) noexcept
        : records_{reinterpret_cast<AggregatorRecord_t*>(
              successorAreaBase + request_demand::getRegionBytes(peerCount))},
          slots_{reinterpret_cast<AggregatedRequest_t*>(
              records_ + getGroupCount(aggregatorGroups, domainCount, peerCount))},
          groups_{getGroupCount(aggregatorGroups, domainCount, peerCount)},
          bitmapBytes_{getBitmapBytes(domainCount)}
    {
    }

    // Members packable per slot: floor(64/bitmapBytes), at least 1.
    [[nodiscard]] static constexpr std::uint32_t
    getMembersPerSlot(std::uint32_t domainCount) noexcept
    {
        const std::uint32_t bitmapBytes = getBitmapBytes(domainCount);
        const std::uint32_t perSlot = (bitmapBytes == 0u) ? 0u : (64u / bitmapBytes);
        return (perSlot == 0u) ? 1u : perSlot;
    }

    // Declaration order is the construction order: slots_ reads records_ and the group count.
    AggregatorRecord_t* records_;
    AggregatedRequest_t* slots_;
    std::uint32_t groups_;
    std::uint32_t bitmapBytes_;
};

static_assert(sizeof(RequestAggLayout::AggregatedRequest_t) == 64, "one cacheline");
static_assert(sizeof(RequestAggLayout::AggregatorRecord_t) == 64, "one cacheline");
static_assert(IsRegionRecord<RequestAggLayout::AggregatedRequest_t>);
static_assert(IsRegionRecord<RequestAggLayout::AggregatorRecord_t>);

}  // namespace cme
