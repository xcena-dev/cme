// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor_request.cpp -- hand-raise SuccessorPolicy (oracle A).
//
// Next owner = first requester with a raised doorbell. Lower latency than
// Order under bursty load. Port of me_bak/src/me_request.c.

#include "core/policy/successor_request.hpp"

#include <chrono>
#include <cstdint>

#include "common/timing.hpp"
#include "config.hpp"
#include "core/algo/ownership_transfer.hpp"
#include "core/domain_bitmap.hpp"
#include "core/policy/request_demand_region.hpp"
#include "core/runtime/local_domain_view.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/event.hpp"
#include "observe/latency.hpp"
#include "observe/observe.hpp"

namespace cme
{

namespace
{

// Hand off @domain to the first clockwise peer requesting it (its demand-region bit
// is set). Returns true on success.
[[nodiscard]] bool transferOwnershipToRequester(LocalPeerState& peerState, DomainId domain)
{
    for (const PeerId peerIndex : peerState.getPeerRing())
    {
        // Demand bit alone, no member read on the grant path. A dead peer's stale demand
        // attracts grants only until the RA scrubs its line at recovery FINISH.
        if (request_demand::loadPeer(peerState, peerIndex).has(domain))
        {
            // Guarded: aborts if a worker re-pinned since the transferability sample, which
            // would otherwise put two threads in the CS.
            return ownership_transfer::transferOwnershipGuarded(peerState, domain, peerIndex);
        }
    }
    return false;
}

// Walk peers clockwise, granting each held domain to the first requester. A requester that
// withdrew mid-scan just receives a domain it will forward on -- no correctness loss.
void transferHeldDomains(LocalPeerState& peerState, DomainBitmap heldDomains)
{
    for (const PeerId peerIndex : peerState.getPeerRing())
    {
        if (heldDomains.isEmpty())
        {
            break;
        }
        DomainBitmap candidates = heldDomains & request_demand::loadPeer(peerState, peerIndex);
        while (!candidates.isEmpty())
        {
            const DomainId domainId = candidates.popLowest();
            // Guarded grant; on abort (a worker re-pinned) stop offering the domain
            // this cycle either way -- it is no longer transferable.
            if (ownership_transfer::transferOwnershipGuarded(peerState, domainId, peerIndex))
            {
                OBSERVE_EVENT(Event::OwnershipTransferOnPoll, peerState);
            }
            heldDomains.clear(domainId);
        }
    }
}

}  // namespace

OwnershipResult RequestPolicy::lock(LocalPeerState& peerState, DomainId domainId, const timing::Deadline& deadline)
{
    // Fast path: already holder (single-peer or cached residency).
    OBSERVE_LATENCY_BEGIN(Resident);
    if (ownership_transfer::holdAndCheckResident(peerState, domainId))
    {
        OBSERVE_LATENCY_END(Resident, peerState, domainId);
        return OwnershipResult::Arrived;
    }

    OBSERVE_LATENCY_BEGIN(Raise);
    request_demand::raise(peerState, domainId);  // the request signal
    OBSERVE_LATENCY_END(Raise, peerState, domainId);
    OwnershipResult result = ownership_transfer::waitForOwnership(peerState, domainId, deadline);
    request_demand::drop(peerState, domainId);
    if (result == OwnershipResult::Arrived)
    {
        return result;
    }

    // Timeout, demand already dropped so no new grant is decided. Settle-wait catches a
    // grant still in flight, which the belief-local poll cycle would not rescue.
    // A new budget on purpose: the caller's is already spent, and this is the extra window the
    // withdraw opens rather than time it was promised.
    result = ownership_transfer::waitForOwnership(peerState, domainId,
                                                  timing::Deadline{RequestWithdrawSettle});
    if (result == OwnershipResult::Arrived)
    {
        return result;
    }

    ownership_transfer::unholdDomain(peerState, domainId);
    return result;
}

void RequestPolicy::unlock(LocalPeerState& peerState, DomainId domainId)
{
    OBSERVE_LATENCY_BEGIN(Unlock);
    LocalDomainView& domain = peerState.getDomain(domainId);

    domain.unpinOwnership();

    if (ownership_transfer::canTransferDomain(peerState, domainId))
    {
        // Pin count hit 0: use phase ended, domain now takeable by others.
        OBSERVE_EVENT(Event::OwnershipTransferable, peerState, domain.getOwnershipPinStart());
        OBSERVE_LATENCY_BEGIN(Scan);
        const bool granted = transferOwnershipToRequester(peerState, domainId);  // ring scan incl. embedded grant
        OBSERVE_LATENCY_END(Scan, peerState, domainId);
        if (granted)
        {
            OBSERVE_EVENT(Event::OwnershipTransferOnRelease, peerState);
        }
    }
    // End the release-path measurement here; what follows is off the handoff path.
    OBSERVE_LATENCY_END(Unlock, peerState, domainId);
}

void RequestPolicy::pollCycle(LocalPeerState& peerState)
{
    // Local belief most cycles; a full FAM reconcile every OwnershipCheckInterval to adopt
    // ownership handed over out-of-band, which the belief misses. The period is well under
    // AcquireTimeout, so a waiter on such a domain still resolves within one lock().
    const bool reconcile = (reconcileCountdown_ == 0);
    reconcileCountdown_ = reconcile ? OwnershipCheckInterval : reconcileCountdown_ - 1;
    const DomainBitmap heldTransferable =
        reconcile ? ownership_transfer::reconcileAndCollectTransferable(peerState)
                  : ownership_transfer::collectTransferableLocal(peerState);

    // Transfer phase: skipped (no member sweep) when nothing is transferable.
    if (!heldTransferable.isEmpty())
    {
        transferHeldDomains(peerState, heldTransferable);
    }
}

void RequestPolicy::leave(LocalPeerState& peerState) noexcept
{
    // Drop our demand line before leaving so holders stop granting to us. Called by
    // ~Peer before leaveMembership (while still active), so we are the sole writer.
    request_demand::clearSelf(peerState);
}

void RequestPolicy::scrubRecoveredPeer(LocalPeerState& peerState, PeerId deadPeerId) noexcept
{
    // The dead peer crashed without dropping its demand (leave() couldn't run). The RA
    // clears the dead peer's demand line so its stale requests can't attract grants. RA
    // is the single writer here; the dead peer won't write it back.
    request_demand::scrub(peerState, deadPeerId);
}

std::uint64_t RequestPolicy::getRegionAreaSize(std::uint32_t /*domainCount*/, std::uint32_t peerCount,
                                               std::uint32_t /*aggregatorGroups*/) const noexcept
{
    // One demand line per peer (the request signal); zeroed by Geometry::format's memset.
    return request_demand::getRegionBytes(peerCount);
}

}  // namespace cme
