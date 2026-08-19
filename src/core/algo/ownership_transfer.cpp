// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// ownership_transfer.cpp -- strategy-agnostic ME primitives.
//
// Port of me_bak/src/me_core.c (sans me_stats wiring; see TODO markers).

#include "core/algo/ownership_transfer.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "common/timing.hpp"
#include "config.hpp"
#include "core/domain_bitmap.hpp"
#include "core/layout/geometry.hpp"
#include "core/layout/geometry_profile.hpp"
#include "core/runtime/local_domain_view.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/event.hpp"
#include "observe/failpoint.hpp"
#include "observe/latency.hpp"
#include "observe/observe.hpp"
#include "util/coherency.hpp"
#include "util/cpu.hpp"
#include "util/endian.hpp"

namespace cme::ownership_transfer
{

namespace
{

// File-private halves; public API: transferOwnership, takeoverOwnership (no wake).

// The baseline only rises. It gates the shadow pre-filter in waitForOwnership, so adopting a
// record older than one we already read past must not re-open a stale group shadow.
void adoptOwnership(LocalDomainView& domain, std::uint64_t newEpoch) noexcept
{
    domain.setLastOwnershipEpoch(std::max(newEpoch, domain.getLastOwnershipEpoch()));
    domain.becomeHolder();
}

// Re-affirm holder belief from the truth record, the sole authority. A group shadow is
// shared and names one peer, so it can distribute the poll but cannot decide holdership.
// Belief only, never the epoch: lastOwnershipEpoch is worker-owned (plain, unsynchronised),
// and consuming the edge here would starve a worker whose wait adopts on it.
void reconcileHolderBelief(LocalPeerState& peerState, DomainId domainId)
{
    auto& domain = peerState.getDomain(domainId);
    if (coherency::get(peerState.getDomainRecord(domainId), peerState.getCoherencyMode()).getHolder() == peerState.getPeerId())
    {
        domain.becomeHolder();
    }
    else
    {
        domain.loseHolder();
    }
}

}  // namespace

void publishDomainRecord(LocalPeerState& peerState, DomainId domainId,
                         const Geometry::DomainRecord_t& record)
{
    coherency::set(peerState.getDomainRecordShadow(domainId, record.getHolder()), record, peerState.getCoherencyMode());
    CME_FAILPOINT_REACH(failpoint::Boundary::TransferBeforeTruth);
    coherency::set(peerState.getDomainRecord(domainId), record, peerState.getCoherencyMode());
}

// Self-seize, stamping self one epoch above @base. The caller supplies the copy to bump:
// peterson the truth it already vacated, recovery the truth raised clear of in-flight shadows.
void takeoverOwnership(LocalPeerState& peerState, DomainId domainId, Geometry::DomainRecord_t base)
{
    base.holder = peerState.getPeerId();
    base.epoch = base.epoch + 1;
    publishDomainRecord(peerState, domainId, base);
    adoptOwnership(peerState.getDomain(domainId), base.epoch);
}

// SWOT Transfer(to): publish the (holder, epoch++) record handoff. No doorbell ring --
// the new holder discovers it by polling the record (waitForOwnership or the poll-cycle
// reconcile).
void transferOwnership(LocalPeerState& peerState, DomainId domainId, PeerId newHolder)
{
    const PeerId selfPeerId = peerState.getPeerId();
    // NoPeer fails the range check.
    if (!peerState.isValidPeer(newHolder))
    {
        return;
    }

    auto record = peerState.loadDomainRecordSnapshot(domainId);
    if (!record.isValidMagic())
    {
        peerState.deactivate();
        return;
    }
    // Only the current holder may publish a transfer (or self-claim).
    if (newHolder != selfPeerId && record.holder != selfPeerId)
    {
        return;
    }

    auto& domain = peerState.getDomain(domainId);

    // Drop holder belief before publishing (self-claim keeps it).
    const bool selfClaim = (newHolder == selfPeerId);
    if (!selfClaim)
    {
        domain.loseHolder();
    }

    // record is single-writer (only the holder publishes), so reuse the snapshot:
    // bump epoch from it (no slot re-read) and store the whole 64B line back.
    const auto newEpoch = record.epoch + 1;
    record.holder = newHolder;
    record.epoch = newEpoch;

    CME_FAILPOINT_REACH(failpoint::Boundary::TransferBeforePublish);
    publishDomainRecord(peerState, domainId, record);
    if (selfClaim)
    {
        adoptOwnership(domain, newEpoch);
    }
}

// The latch CAS is the atomic "pin still 0?" re-check a plain canTransferDomain sample
// cannot give: a resident fast-path either wins the pin and aborts the grant, or hits the
// latch and spins until the publish lands.
bool transferOwnershipGuarded(LocalPeerState& peerState, DomainId domainId, PeerId newHolder)
{
    auto& domain = peerState.getDomain(domainId);
    if (!domain.tryBeginOwnershipTransfer())
    {
        return false;  // a worker re-pinned since the grant decision
    }
    transferOwnership(peerState, domainId, newHolder);
    domain.endOwnershipTransfer();
    return true;
}

// Model-A unlock helper: stamp record NoPeer+epoch before tournament release.
// Must run before release() so a new winner self-stamping can't be clobbered.
void vacateOwnership(LocalPeerState& peerState, DomainId domainId)
{
    // rmw: fresh read (rmb) so epoch advances from FAM truth, not a stale DRAM copy.
    // Single-writer (holder) -> rmb + mutate + wmb in place.
    coherency::rmw(peerState.getDomainRecord(domainId), peerState.getCoherencyMode(),
                   [](auto* rec)
                   {
                       rec->holder = NoPeer;
                       rec->epoch = rec->epoch + 1;
                   });
    peerState.getDomain(domainId).loseHolder();
}

void bumpHeartbeat(LocalPeerState& peerState)
{
    // Self owns its slot: stamp the local truth's witness, then write through (no rmb).
    peerState.stampSelfSeen();
    peerState.publishSelfMemberState();

    // publishProfile gates itself (build toggle + null + magic) and barriers its own read.
    const auto& telemetry = peerState.getTelemetry();
    auto* profileSlot = peerState.getProfileSlot(peerState.getPeerId());
    publishProfile(peerState.getCoherencyMode(), profileSlot,
                   telemetry.ownership.time.spin.load(std::memory_order_relaxed),
                   telemetry.ownership.time.wait.load(std::memory_order_relaxed));
}

OwnershipResult waitForOwnership(LocalPeerState& peerState, DomainId domainId,
                                 const timing::Deadline& deadline)
{
    const PeerId selfPeerId = peerState.getPeerId();
    auto& domain = peerState.getDomain(domainId);

    if (domain.isHolder())
    {
        const auto record = peerState.loadDomainRecordSnapshot(domainId);
        if (record.isValidMagic() && record.getHolder() == selfPeerId)
        {
            OBSERVE_EVENT(Event::OwnershipAlreadyHave, peerState);
            return OwnershipResult::Arrived;
        }
        // The snapshot disproved the belief, so drop it here rather than carry it into the
        // wait: the loop adopts on an epoch edge and re-tests the belief nowhere.
        domain.loseHolder();
    }

    const timing::Stopwatch waited;
    const auto cpuStart = OBSERVE_CPU_TIME();       // syscall only under stats/profile; else zero
    const timing::Deadline spinWindow{SpinWindow};  // hot phase boundary (time-based)

    // Adaptive spin backoff: start tight (catch a near token fast), widen the
    // inter-poll gap as the wait drags (cuts cacheline/FAM traffic).
    cpu::SpinBackoff backoff{SpinPausesMin, SpinPausesMax};

    // Poll our group's shadow rather than the shared truth: spreading the wait across
    // shadows avoids a read storm on one line. epoch>last also rejects a stale read.
    auto* selfShadow = peerState.getDomainRecordShadow(domainId, selfPeerId);
    OBSERVE_LATENCY_BEGIN(Spin);
    // One clock read per turn, asked of both budgets. Letting each read its own would double the
    // reads in the hottest loop in the library.
    for (auto now = timing::now(); !deadline.expiredAt(now); now = timing::now())
    {
        const bool spinning = !spinWindow.expiredAt(now);

        // Confirm against the truth only on a fresh edge, since the truth is the sole
        // holder authority and the shadow read is the cheap distributed filter.
        OBSERVE_LATENCY_BEGIN(SpinPoll);
        const auto shadow = coherency::get(selfShadow, peerState.getCoherencyMode());
        OBSERVE_LATENCY_END_COUNT(SpinPoll, peerState);  // counter only; a per-poll span would flood the JSONL
        // Cost filter, not the decision: a group shadow keeps naming us after the domain moved
        // to another group, so the epoch is what stops one truth read per poll on that line.
        // Magic first: a corrupt line's holder and epoch are garbage too, and both would read
        // as an edge here. Free -- the 64B copy is in hand.
        if (shadow.isValidMagic() && shadow.getHolder() == selfPeerId &&
            shadow.epoch > domain.getLastOwnershipEpoch())
        {
            // The truth is the sole holder authority, so its holder alone settles this: an epoch at
            // or below the baseline still names us.
            const auto truth = peerState.loadDomainRecordSnapshot(domainId);
            if (truth.isValidMagic() && truth.getHolder() == selfPeerId)
            {
                OBSERVE_LATENCY_END(Spin, peerState, domainId);
                OBSERVE_LATENCY_BEGIN(AdoptLocal);
                adoptOwnership(domain, truth.epoch);
                OBSERVE_LATENCY_END(AdoptLocal, peerState, domainId);
                OBSERVE_EVENT(Event::OwnershipArrived, peerState, waited, cpuStart, true);
                return OwnershipResult::Arrived;
            }
        }

        if (spinning)
        {
            cpu::relaxCpu(backoff.next());
        }
        else
        {
            cpu::sleepFor(peerState.getPollInterval() / 2 + timing::Micros{1});
        }
    }

    OBSERVE_EVENT(Event::OwnershipNotArrived, peerState, waited, cpuStart);
    return OwnershipResult::NotArrived;
}

DomainBitmap reconcileAndCollectTransferable(LocalPeerState& peerState)
{
    DomainBitmap transferableDomains;

    for (const DomainId domainId : peerState.getActiveDomainRange())
    {
        // A worker is in lock() waiting to adopt this token itself; grabbing it here would
        // forward that worker's incoming grant away and starve it.
        if (peerState.isPendingDomain(domainId))
        {
            continue;
        }

        const PeerId selfPeerId = peerState.getPeerId();
        auto& domain = peerState.getDomain(domainId);

        // The check turn is the only abdication point, so it reads the truth. Off-turn the
        // shadow is a pre-filter for an arriving grant; naming a group-mate is ignored,
        // since a desynced shared line must never drive abdication.
        if (domain.isTurnToCheckOwnership() ||
            coherency::get(peerState.getDomainRecordShadow(domainId, selfPeerId), peerState.getCoherencyMode()).getHolder() == selfPeerId)
        {
            reconcileHolderBelief(peerState, domainId);
        }

        if (domain.isHolder() && ownership_transfer::canTransferDomain(peerState, domainId))
        {
            transferableDomains.set(domainId);
        }
    }

    return transferableDomains;
}

// Local belief only, dropping the O(domains) UC read that dominated REQUEST's poll cycle. A
// grant only reaches a peer that raised demand and that peer adopts it itself, so no sweep is
// needed; transferOwnership re-reads the record anyway and aborts if we no longer hold it.
DomainBitmap collectTransferableLocal(LocalPeerState& peerState)
{
    DomainBitmap transferableDomains;
    for (const DomainId domainId : peerState.getActiveDomainRange())
    {
        if (peerState.isPendingDomain(domainId))
        {
            continue;
        }
        if (peerState.getDomain(domainId).isHolder() && canTransferDomain(peerState, domainId))
        {
            transferableDomains.set(domainId);
        }
    }
    return transferableDomains;
}

void holdDomain(LocalPeerState& peerState, DomainId domainId)
{
    peerState.getDomain(domainId).pinOwnership();
    OBSERVE_EVENT(Event::OwnershipRequested, peerState, domainId);
}

bool holdAndCheckResident(LocalPeerState& peerState, DomainId domainId)
{
    holdDomain(peerState, domainId);
    auto& domain = peerState.getDomain(domainId);
    // DRAM-first filter: without local holder belief there is no fast path, so skip the
    // FAM read entirely.
    if (!domain.isHolder())
    {
        return false;
    }
    // Belief can be stale (taken over, torn handoff), so confirm against a fresh snapshot.
    const auto record = peerState.loadDomainRecordSnapshot(domainId);
    if (record.isValidMagic() && record.getHolder() == peerState.getPeerId())
    {
        return true;
    }
    // Disproved: drop it so the poll cycle stops offering a domain we do not hold. The read
    // is already paid for here; collectTransferableLocal has no truth read to catch it.
    domain.loseHolder();
    return false;
}

void unholdDomain(LocalPeerState& peerState, DomainId domainId)
{
    auto& domain = peerState.getDomain(domainId);
    OBSERVE_EVENT(Event::OwnershipTransferable, peerState, domain.getOwnershipPinStart());
    domain.unpinOwnership();
}

bool canTransferDomain(LocalPeerState& peerState, DomainId domainId) noexcept
{
    // No sweep pin: the orphan-sweep lands via SweepControlTimeout even while ORDER keeps
    // forwarding control, so the token need not be frozen on the RA.
    return peerState.getDomain(domainId).canTransferOwnership();
}

}  // namespace cme::ownership_transfer
