// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor_request_agg.cpp -- hand-raise SuccessorPolicy with packed
// demand aggregation (oracle A variant).
//
// Same protocol as RequestPolicy; only the successor scan differs. Peers are partitioned
// into G groups (p -> p % G), each with an aggregator that packs its members' wanted-domain
// bitmaps off the critical path. The holder trusts the packed slot as-is: it reads neither the
// aggregator record nor the member lines the slot stands for, so there is no liveness check and
// no direct-member fallback. A crashed aggregator's group therefore keeps the last packed
// content until recovery re-elects one, and demand raised in that window attracts no grant.

#include "core/policy/successor_request_agg.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "config.hpp"
#include "core/algo/ownership_transfer.hpp"
#include "core/domain_bitmap.hpp"
#include "core/layout/geometry.hpp"
#include "core/policy/request_demand_region.hpp"
#include "core/policy/successor_request_agg_layout.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/event.hpp"
#include "observe/observe.hpp"
#include "util/coherency.hpp"
#include "util/endian.hpp"
#include "util/util.hpp"

namespace cme
{

namespace
{

// First active member of group @groupId (peerId % G == groupId, strided), excluding
// @excludePeer; NoPeer if none remain. Shared by leave() (planned handoff) and
// scrubRecoveredPeer() (crash handoff).
[[nodiscard]] PeerId findLiveGroupAggregator(LocalPeerState& peerState, std::uint32_t groups,
                                             std::uint32_t groupId, PeerId excludePeer) noexcept
{
    const std::uint32_t peerCount = peerState.getMaxPeers();
    for (std::uint32_t peerId = groupId; peerId < peerCount; peerId += groups)
    {
        if (peerId == excludePeer)
        {
            continue;
        }
        if (peerState.loadMemberSnapshot(peerId).hasStatus(Geometry::Member_t::Status::Active))
        {
            return peerId;
        }
    }
    return NoPeer;
}

// ── packed codec ───────────────────────────────────────────────────────────

[[nodiscard]] DomainBitmap unpackAggregatedRequest(const RequestAggLayout::AggregatedRequest_t& slot,
                                                   std::uint32_t slotLocalIndex,
                                                   std::uint32_t bitmapBytes) noexcept
{
    DomainBitmap wanted;  // zero-initialised; only bitmapBytes get overwritten
    const std::uint64_t offset = static_cast<std::uint64_t>(slotLocalIndex) * bitmapBytes;
    std::memcpy(wanted.getData(), &slot.packedBitmaps[offset], bitmapBytes);
    return wanted;
}

// Pack group @groupId's members (peerId % G == groupId, strided) into one slot.
[[nodiscard]] RequestAggLayout::AggregatedRequest_t packAggregatedRequest(LocalPeerState& peerState, std::uint32_t groups,
                                                                          std::uint32_t bitmapBytes, std::uint32_t groupId)
{
    const std::uint32_t peerCount = peerState.getMaxPeers();

    RequestAggLayout::AggregatedRequest_t aggregated{};
    for (std::uint32_t peerId = groupId; peerId < peerCount; peerId += groups)
    {
        const std::uint32_t slotLocalIndex = peerId / groups;
        // Aggregation runs on the poll thread (self-retrying); read the request signal from
        // the per-peer demand region.
        const DomainBitmap wanted = request_demand::loadPeer(peerState, peerId);
        const std::uint64_t offset = static_cast<std::uint64_t>(slotLocalIndex) * bitmapBytes;
        std::memcpy(&aggregated.packedBitmaps[offset], wanted.getData(), bitmapBytes);
    }
    return aggregated;
}

// ── handoff helpers ────────────────────────────────────────────────────────
// Grant held domains to @peerIndex on its demand bits alone -- one FAM line, no member read,
// same contract as RequestPolicy. Clears granted domains from @heldDomains.
bool grantHeldToPeer(LocalPeerState& peerState, std::uint32_t peerIndex, DomainBitmap& heldDomains)
{
    DomainBitmap candidates = heldDomains & request_demand::loadPeer(peerState, peerIndex);
    bool granted = false;
    while (!candidates.isEmpty())
    {
        const DomainId domainId = candidates.popLowest();
        // On abort (a worker re-pinned) the domain is not offerable this cycle either way.
        if (ownership_transfer::transferOwnershipGuarded(peerState, domainId, peerIndex))
        {
            granted = true;
            OBSERVE_EVENT(Event::OwnershipTransferOnPoll, peerState);
        }
        heldDomains.clear(domainId);
    }
    return granted;
}

}  // namespace

void RequestAggPolicy::bind(LocalPeerState& peerState) noexcept
{
    const RequestAggLayout& layout = layout_.emplace(peerState);
    groupId_ = peerState.getPeerId() % layout.getGroups();
    slotLocalIndex_ = peerState.getPeerId() / layout.getGroups();
}

// Scan groups holder-relative for fairness, so grants rotate instead of favouring low ids.
// The packed slot narrows candidates before the per-member confirm and is trusted as-is --
// a vacant or crashed aggregator is healed within one poll tick.
bool RequestAggPolicy::transferHeldDomainsAgg(LocalPeerState& peerState, DomainBitmap heldDomains)
{
    const RequestAggLayout& layout = getLayout();
    const std::uint32_t selfPeerId = peerState.getPeerId();
    const std::uint32_t peerCount = peerState.getMaxPeers();
    const std::uint32_t groups = layout.getGroups();
    const std::uint32_t bitmapBytes = layout.getBitmapBytes();
    const std::uint32_t selfGroupId = groupId_;

    bool granted = false;
    for (std::uint32_t groupStep = 0; groupStep < groups; ++groupStep)
    {
        if (heldDomains.isEmpty())
        {
            break;
        }
        const std::uint32_t groupId = (selfGroupId + groupStep) % groups;
        if (groupId >= peerCount)
        {
            continue;  // empty group (more groups than peers)
        }
        const std::uint32_t memberCount = ceilDiv(peerCount - groupId, groups);

        const RequestAggLayout::AggregatedRequest_t slot = coherency::get(layout.getSlot(groupId), peerState.getCoherencyMode());

        // Own group: start just after this peer's slot-local index; others: from 0.
        const std::uint32_t startLocalIndex = (groupId == selfGroupId) ? (slotLocalIndex_ + 1) : 0;
        for (std::uint32_t memberStep = 0; memberStep < memberCount; ++memberStep)
        {
            const std::uint32_t slotLocalIndex = (startLocalIndex + memberStep) % memberCount;
            const std::uint32_t peerId = groupId + slotLocalIndex * groups;
            if (peerId == selfPeerId)
            {
                continue;
            }
            // Skip peers the slot shows no demand for; grantHeldToPeer re-confirms the
            // survivors against the fresh demand line.
            const DomainBitmap wanted = unpackAggregatedRequest(slot, slotLocalIndex, bitmapBytes);
            if ((heldDomains & wanted).isEmpty())
            {
                continue;
            }
            granted = grantHeldToPeer(peerState, peerId, heldDomains) || granted;
        }
    }
    return granted;
}

// ── aggregation duty ───────────────────────────────────────────────────────
// Refresh this peer's group slot iff the record names it aggregator, claiming a vacant duty
// first.
void RequestAggPolicy::refreshAggregatedRequest(LocalPeerState& peerState)
{
    const RequestAggLayout& layout = getLayout();
    const std::uint32_t selfPeerId = peerState.getPeerId();
    auto* record = layout.getRecord(groupId_);
    auto snapshot = coherency::get(record, peerState.getCoherencyMode());
    // Vacant duty (NoPeer, or a named peer now absent): volunteer self. Bare LWW suffices --
    // concurrent volunteers converge next tick, and a rival's content cannot outlive one tick
    // because the survivor republishes the whole slot unconditionally below.
    if (snapshot.aggregatorPeerId != selfPeerId &&
        (!peerState.isValidPeer(snapshot.aggregatorPeerId) ||
         !peerState.getMemberView(snapshot.aggregatorPeerId, MemberCacheTTL).active))
    {
        snapshot.aggregatorPeerId = selfPeerId;
        coherency::set(record, snapshot, peerState.getCoherencyMode());
    }
    if (snapshot.aggregatorPeerId != selfPeerId)
    {
        return;  // not this group's aggregator
    }
    const RequestAggLayout::AggregatedRequest_t aggregated =
        packAggregatedRequest(peerState, layout.getGroups(), layout.getBitmapBytes(), groupId_);

    // Unconditional. Eliding the store on "unchanged since my own last write" froze the slot on
    // a rival volunteer's content: our copy kept matching, so we never republished.
    coherency::set(layout.getSlot(groupId_), aggregated, peerState.getCoherencyMode());
}

// ── request-time slice publish ─────────────────────────────────────────────
// Raise/drop this peer's demand and carry the change into the packed slot in the same step, so
// a new request is visible to holders without waiting for the aggregator's next tick -- or, if
// that aggregator has crashed, for recovery to elect another. Both writes run under
// updateSelfPending's mutex, so two workers of this peer cannot interleave their slices.
void RequestAggPolicy::updateDemandAndSlice(LocalPeerState& peerState, DomainId domainId,
                                            bool requesting) noexcept
{
    const RequestAggLayout& layout = getLayout();
    peerState.updateSelfPending(
        domainId, requesting,
        [&](const DomainBitmap& bits) noexcept
        {
            // Demand line first: a holder that sees the slice re-confirms against this line,
            // and a slice visible ahead of it costs that peer the tick's grant.
            request_demand::storePending(peerState.getSuccessorAreaBase(), peerState.getPeerId(),
                                         bits, peerState.getCoherencyMode());
            // rmw, not a store of our own bytes: the slices are byte-disjoint, but wmb flushes a
            // whole line, so under Flush a narrow write would rewrite a group-mate's stale copy.
            // A slice lost to a concurrent rmw returns on refreshAggregatedRequest's next tick.
            coherency::rmw(layout.getSlot(groupId_), peerState.getCoherencyMode(),
                           [&](RequestAggLayout::AggregatedRequest_t* slot) noexcept
                           {
                               const std::uint64_t offset =
                                   static_cast<std::uint64_t>(slotLocalIndex_) * layout.getBitmapBytes();
                               std::memcpy(&slot->packedBitmaps[offset], bits.getData(),
                                           layout.getBitmapBytes());
                           });
        });
}

OwnershipResult RequestAggPolicy::lock(LocalPeerState& peerState, DomainId domainId,
                                       const timing::Deadline& deadline)
{
    // Fast path: already holder (single-peer or cached residency).
    if (ownership_transfer::holdAndCheckResident(peerState, domainId))
    {
        return OwnershipResult::Arrived;
    }

    updateDemandAndSlice(peerState, domainId, true);  // the request signal + its slice
    const OwnershipResult result = ownership_transfer::waitForOwnership(peerState, domainId, deadline);
    // Plain drop, no slice write. A slice late to clear attracts one grant this peer forwards on
    // (harmless, see transferHeldDomains); a slice late to appear costs a full AcquireTimeout. The
    // cost is the same rmw either way, so only the raise pays it.
    request_demand::drop(peerState, domainId);
    if (result == OwnershipResult::Arrived)
    {
        return result;
    }

    ownership_transfer::unholdDomain(peerState, domainId);
    return result;
}

void RequestAggPolicy::unlock(LocalPeerState& peerState, DomainId domainId)
{
    auto& domain = peerState.getDomain(domainId);

    domain.unpinOwnership();

    if (ownership_transfer::canTransferDomain(peerState, domainId))
    {
        // Pin count hit 0: use phase ended, domain now takeable by others.
        OBSERVE_EVENT(Event::OwnershipTransferable, peerState, domain.getOwnershipPinStart());
        DomainBitmap released;
        released.set(domainId);
        if (transferHeldDomainsAgg(peerState, released))
        {
            OBSERVE_EVENT(Event::OwnershipTransferOnRelease, peerState);
        }
    }
}

void RequestAggPolicy::pollCycle(LocalPeerState& peerState)
{
    // Aggregator duty: refresh this peer's group slot (off the hot path).
    refreshAggregatedRequest(peerState);

    // Mostly local-belief collect; periodic FAM reconcile adopts out-of-band ownership
    // (see RequestPolicy::pollCycle).
    const bool reconcile = (reconcileCountdown_ == 0);
    reconcileCountdown_ = reconcile ? OwnershipCheckInterval : reconcileCountdown_ - 1;
    const DomainBitmap heldTransferable =
        reconcile ? ownership_transfer::reconcileAndCollectTransferable(peerState)
                  : ownership_transfer::collectTransferableLocal(peerState);
    if (!heldTransferable.isEmpty())
    {
        // Whether anything was granted is unlock()'s business, which reports it as an event.
        // A poll tick grants when it can and says nothing either way.
        static_cast<void>(transferHeldDomainsAgg(peerState, heldTransferable));
    }
}

std::uint64_t RequestAggPolicy::getRegionAreaSize(std::uint32_t domainCount, std::uint32_t peerCount,
                                                  std::uint32_t aggregatorGroups) const noexcept
{
    return RequestAggLayout::getAreaBytes(domainCount, peerCount, aggregatorGroups);
}

void RequestAggPolicy::format(std::uint8_t* successorAreaBase, std::uint32_t domainCount,
                              std::uint32_t peerCount, std::uint32_t aggregatorGroups,
                              CoherencyMode /*mode*/) const noexcept
{
    // Initial aggregator of group g = peer g (strided group g's first member).
    // format() publishes the whole area, so no fence here. Demand lines + request slots
    // stay zero (memset). Aggregator records follow the leading per-peer demand region.
    const RequestAggLayout layout{successorAreaBase, domainCount, peerCount, aggregatorGroups};
    for (std::uint32_t groupId = 0; groupId < layout.getGroups(); ++groupId)
    {
        auto* record = layout.getRecord(groupId);
        record->magic = RequestAggLayout::AggregatorRecord_t::Magic;
        record->aggregatorPeerId = groupId;
    }
}

void RequestAggPolicy::leave(LocalPeerState& peerState) noexcept
{
    const RequestAggLayout& layout = getLayout();
    // Drop our demand line so holders stop granting to us (same as RequestPolicy::leave).
    request_demand::clearSelf(peerState);

    // If this peer is its group's aggregator, hand the duty to a live group member
    // (NoPeer if none remain -> holder falls back to a direct scan).
    const std::uint32_t selfPeerId = peerState.getPeerId();
    auto* record = layout.getRecord(groupId_);
    auto snapshot = coherency::get(record, peerState.getCoherencyMode());  // one 64B read serves the check + write base
    if (snapshot.aggregatorPeerId != selfPeerId)
    {
        return;
    }
    snapshot.aggregatorPeerId = findLiveGroupAggregator(peerState, layout.getGroups(), groupId_, selfPeerId);
    coherency::set(record, snapshot, peerState.getCoherencyMode());  // single-writer (named aggregator): whole-line store + wmb
}

void RequestAggPolicy::scrubRecoveredPeer(LocalPeerState& peerState, PeerId deadPeerId) noexcept
{
    const RequestAggLayout& layout = getLayout();
    // Scrub the dead peer's demand line (same as RequestPolicy) so its stale requests
    // can't attract grants. RA is the single writer; the dead peer won't write it back.
    request_demand::scrub(peerState, deadPeerId);

    // The crash skipped leave(), so re-elect a live member and take the group out of
    // holder-fallback. Only one group's record can name the dead peer, and RA is its writer.
    // The dead peer's group, not ours: groupId_ is this peer's own and does not apply here.
    const std::uint32_t deadGroupId = deadPeerId % layout.getGroups();
    auto* record = layout.getRecord(deadGroupId);
    auto snapshot = coherency::get(record, peerState.getCoherencyMode());  // one 64B read serves the check + write base
    if (snapshot.aggregatorPeerId != deadPeerId)
    {
        return;  // dead peer wasn't this group's aggregator -> nothing to hand off
    }
    snapshot.aggregatorPeerId = findLiveGroupAggregator(peerState, layout.getGroups(), deadGroupId, deadPeerId);
    coherency::set(record, snapshot, peerState.getCoherencyMode());  // single-writer (named aggregator): whole-line store + wmb
}

}  // namespace cme
