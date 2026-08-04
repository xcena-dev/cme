// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor_order.cpp -- ring-order SuccessorPolicy (oracle A).
//
// Next owner = next ACTIVE peer in ring order. Release hands off to the next
// alive peer when pin count hits 0; poll thread eagerly forwards likewise.
// Port of me_bak/src/me_order.c.

#include "core/policy/successor_order.hpp"

#include <cerrno>

#include "config.hpp"
#include "core/algo/ownership_transfer.hpp"
#include "core/algo/peer.hpp"
#include "core/domain_bitmap.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/event.hpp"
#include "observe/latency.hpp"
#include "observe/observe.hpp"

namespace cme
{

namespace
{

// First ACTIVE peer clockwise after @from participating in @domainId; @from if
// none. Independent of ChainRecoveryAuthorityPolicy (distinct concerns).
[[nodiscard]] PeerId findSuccessor(LocalPeerState& peerState, PeerId from, DomainId domainId)
{
    for (const PeerId candidate : IdRing<PeerId>(from, peerState.getPeerScanBound()))
    {
        // Cached (bounded-stale) member view: amortizes the per-hop FAM read. A stale pick
        // can strand the token on a peer that just left; recovery FSM reclaims it.
        const auto member = peerState.getMemberView(candidate, MemberCacheTTL);
        if (member.active && member.participating.has(domainId))
        {
            return candidate;
        }
    }
    return from;
}

void transferHeldDomains(LocalPeerState& peerState, DomainBitmap heldDomains)
{
    for (const PeerId peerIndex : peerState.getPeerRing())
    {
        if (heldDomains.isEmpty())
        {
            break;
        }
        // Cached (bounded-stale) member view, matching findSuccessor. A stale pick can
        // strand the token on a peer that just left; recovery FSM reclaims it.
        const auto member = peerState.getMemberView(peerIndex, MemberCacheTTL);
        if (!member.active)
        {
            continue;
        }
        DomainBitmap candidates = heldDomains & member.participating;
        while (!candidates.isEmpty())
        {
            const DomainId domainId = candidates.popLowest();
            // Eager forward: unconditional grant to the next clockwise active
            // participant (token-ring handoff, no requester needed). Guarded so a
            // local worker's resident re-pin since the collect aborts the grant.
            if (ownership_transfer::transferOwnershipGuarded(peerState, domainId, peerIndex))
            {
                OBSERVE_EVENT(Event::OwnershipTransferOnPoll, peerState);
            }
            heldDomains.clear(domainId);
        }
    }
}

}  // namespace

OwnershipResult OrderPolicy::lock(LocalPeerState& peerState, DomainId domainId, std::chrono::nanoseconds timeout)
{
    OBSERVE_LATENCY_BEGIN(Hold);
    ownership_transfer::holdDomain(peerState, domainId);
    OBSERVE_LATENCY_END(Hold, peerState, domainId);
    const OwnershipResult result = ownership_transfer::waitForOwnership(peerState, domainId, timeout);
    if (result == OwnershipResult::Arrived)
    {
        return result;
    }

    ownership_transfer::unholdDomain(peerState, domainId);
    return result;
}

void OrderPolicy::unlock(LocalPeerState& peerState, DomainId domainId)
{
    OBSERVE_LATENCY_BEGIN(Unlock);
    auto& domain = peerState.getDomain(domainId);

    domain.unpinOwnership();

    if (ownership_transfer::canTransferDomain(peerState, domainId))
    {
        // Pin count hit 0: use phase ended, domain now takeable by others.
        OBSERVE_EVENT(Event::OwnershipTransferable, peerState, domain.getOwnershipPinStart());

        OBSERVE_LATENCY_BEGIN(Scan);
        const PeerId successor = findSuccessor(peerState, peerState.getPeerId(), domainId);
        OBSERVE_LATENCY_END(Scan, peerState, domainId);
        if (successor != peerState.getPeerId() &&
            ownership_transfer::transferOwnershipGuarded(peerState, domainId, successor))
        {
            OBSERVE_EVENT(Event::OwnershipTransferOnRelease, peerState);
        }
    }
    OBSERVE_LATENCY_END(Unlock, peerState, domainId);
}

void OrderPolicy::pollCycle(LocalPeerState& peerState)
{
    // Collect phase: build set of held+transferable domains.
    const DomainBitmap heldTransferable = ownership_transfer::reconcileAndCollectTransferable(peerState);

    // Transfer phase: skipped (no member sweep) when nothing is transferable.
    if (!heldTransferable.isEmpty())
    {
        transferHeldDomains(peerState, heldTransferable);
    }
}

}  // namespace cme
