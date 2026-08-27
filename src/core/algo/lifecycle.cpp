// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// lifecycle.cpp -- peer membership lifecycle orchestration (join / leave / rejoin).

#include "core/algo/lifecycle.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>

#include "common/timing.hpp"
#include "core/algo/ownership_transfer.hpp"
#include "core/domain_bitmap.hpp"
#include "core/layout/geometry.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/failpoint.hpp"
#include "util/coherency.hpp"

namespace cme::lifecycle
{

namespace
{

// Prime sequencing baselines from the shared record; adopt holder belief iff the
// record names us. Returns false if the record line is uninitialised.
[[nodiscard]] bool syncDomainView(LocalPeerState& peerState, DomainId domainId)
{
    const PeerId selfPeerId = peerState.getPeerId();
    const auto domainRecord = peerState.loadDomainRecordSnapshot(domainId);  // get (rmb)

    if (!domainRecord.isValidMagic())
    {
        return false;
    }

    auto& domain = peerState.getDomain(domainId);

    domain.setLastOwnershipEpoch(domainRecord.epoch);

    if (domainRecord.getHolder() == selfPeerId)
    {
        domain.becomeHolder();  // record names us -- restore belief
    }
    else
    {
        domain.loseHolder();
    }

    return true;
}

// First active participant of @domainId other than self; NoPeer if sole participant.
// leaveDomain uses NoPeer as its sole-participant predicate, so this must stay
// participation-only -- getFallbackHolderOnLeave is the separate "anyone alive" question.
[[nodiscard]] PeerId getSuccessorOnLeave(LocalPeerState& peerState, DomainId domainId)
{
    for (const PeerId peerId : peerState.getPeerRing())
    {
        const auto memberSlot = peerState.loadMemberSnapshot(peerId);  // get (rmb)
        if (memberSlot.hasStatus(Geometry::Member_t::Status::Active) &&
            memberSlot.loadParticipatingDomains().has(domainId))
        {
            return peerId;
        }
    }
    return NoPeer;
}

// Any Active peer other than self; NoPeer if none is left. A holder to park the domain on,
// not a successor: the poll scan scope is the Header activeDomains bitmap rather than
// participation, so a non-participant still forwards the domain on demand, and an Active peer
// that later dies is a recovery target -- a departed (None) slot never is.
[[nodiscard]] PeerId getFallbackHolderOnLeave(LocalPeerState& peerState)
{
    for (const PeerId peerId : peerState.getPeerRing())
    {
        if (peerState.loadMemberSnapshot(peerId).hasStatus(Geometry::Member_t::Status::Active))
        {
            return peerId;
        }
    }
    return NoPeer;
}

// First Free data domain entry; also reclaims mid-write aborts (left Free).
// We hold control, so no concurrent writer. NoDomain if all Active.
[[nodiscard]] DomainId findEmptyDataDomain(LocalPeerState& peerState)
{
    for (const DomainId domainId : peerState.getDataDomainIdRange())
    {
        const auto record = peerState.loadDomainRecordSnapshot(domainId);  // get (rmb)
        if (record.isValidMagic() &&
            !record.hasState(Geometry::DomainRecord_t::State::Active))
        {
            return domainId;
        }
    }
    return NoDomain;
}

// True if any active peer other than self participates in @domainId.
[[nodiscard]] bool hasOtherParticipant(LocalPeerState& peerState, DomainId domainId)
{
    const PeerId selfPeerId = peerState.getPeerId();
    for (const PeerId peerId : peerState.getPeerIdRange())
    {
        if (peerId == selfPeerId)
        {
            continue;
        }
        const auto memberSlot = peerState.loadMemberSnapshot(peerId);  // get (rmb)
        if (memberSlot.hasStatus(Geometry::Member_t::Status::Active) &&
            memberSlot.loadParticipatingDomains().has(domainId))
        {
            return true;
        }
    }
    return false;
}

}  // namespace

DomainId findDataDomainByName(const LocalPeerState& peerState, std::string_view name,
                              std::uint64_t& outIncarnation)
{
    for (const DomainId domainId : peerState.getDataDomainIdRange())
    {
        const auto record = peerState.loadDomainRecordSnapshot(domainId);  // get (rmb)
        if (record.isValidMagic() &&
            record.hasState(Geometry::DomainRecord_t::State::Active) &&
            record.getName() == name)
        {
            // From this snapshot, not a second read of the slot: the match and the incarnation have to
            // come from one visit or they can describe two different domains.
            outIncarnation = record.generation;
            return domainId;
        }
    }
    outIncarnation = 0;
    return NoDomain;
}

// Zero for a slot that is not a live domain, which is a value no live incarnation has: createDomain
// bumps generation before it flips the slot Active.
std::uint64_t readDomainIncarnation(const LocalPeerState& peerState, DomainId domainId)
{
    const auto record = peerState.loadDomainRecordSnapshot(domainId);  // get (rmb)
    if (!record.isValidMagic() || !record.hasState(Geometry::DomainRecord_t::State::Active))
    {
        return 0;
    }
    return record.generation;
}

JoinResult joinMembership(LocalPeerState& peerState)
{
    // TLA+ SWOT_Rejoin: baseline all domains BEFORE status=Active --
    // no peer may forward to us with stale lastHandoffSeq / lastOwnershipEpoch.
    const PeerId selfPeerId = peerState.getPeerId();
    auto* memberSlot = peerState.getMemberSlot(selfPeerId);
    // The only self FAM read: seed the local copy once, then write through. join runs before
    // the poll thread starts, so the whole-slot set is race-free.
    auto selfState = coherency::get(memberSlot, peerState.getCoherencyMode());
    if (!selfState.isValidMagic())
    {
        peerState.deactivate();
        return JoinResult::CorruptRegion;  // own member slot uninitialised / corrupted region
    }
    selfState.lastSeenNanos = timing::wall<timing::Nanos>();  // seen at join; poll thread re-stamps
    // Seed participation with the control domain only -- mandatory for create/delete; data
    // domains are joined explicitly. The policy-private demand line is already clean.
    DomainBitmap participating;
    participating.set(ControlDomainId);
    selfState.storeParticipatingDomains(participating);
    peerState.seedSelfMemberState(selfState);  // store the fully-initialised member, then write through
    peerState.publishSelfMemberState();

    CME_FAILPOINT_REACH(failpoint::Boundary::JoinBeforeBaseline);

    // Sync every domain's DRAM view; joiner adopts or defers to existing holder.
    for (const DomainId domainId : peerState.getDomainIdRange())
    {
        if (!syncDomainView(peerState, domainId))
        {
            peerState.deactivate();
            return JoinResult::CorruptRegion;  // per-domain SWPC line uninitialised / corrupted
        }
    }

    // Per-domain state consistent: flip to Active (TLA+ RejoinResync:
    // rejoining -> ~rejoining) so other peers may forward/target this peer.
    peerState.getSelfMemberState().setStatus(Geometry::Member_t::Status::Active);
    peerState.publishSelfMemberState();
    return JoinResult::Ok;
}

void leaveMembership(LocalPeerState& peerState) noexcept
{
    const PeerId selfPeerId = peerState.getPeerId();

    // None goes last: no RA targets a None slot, so a record still naming us would strand. Leaving
    // covers the sweep -- MemberView.active tests status == Active exactly, and isAlive accepts it.

    // Domain-independent, so scan the ring once rather than per domain.
    const PeerId fallbackHolder = getFallbackHolderOnLeave(peerState);

    // One pass. Sweeping again catches nothing: a sweep is sub-microsecond at low domain counts, so
    // it never spans a late grant's arrival. The drain is what covers that.
    for (const DomainId domainId : peerState.getDomainIdRange())
    {
        const auto domainRecord = peerState.loadDomainRecordSnapshot(domainId);  // get (rmb)

        // Spare slots have holder=NoPeer; identity check skips them.
        if (domainRecord.getHolder() == selfPeerId)
        {
            // Participant first, else park on anyone alive. Peer 0 when no Active peer is left
            // is correct, not a fallback: reserveMemberSlot hands the lowest free slot to the
            // next joiner, so that joiner IS peer 0 and syncDomainView adopts what it inherits.
            // Vacating instead is worse -- only Peterson can seize a holderless record.
            PeerId successor = getSuccessorOnLeave(peerState, domainId);
            if (isNoPeer(successor))
            {
                successor = fallbackHolder;
            }
            ownership_transfer::transferOwnership(peerState, domainId,
                                                  isNoPeer(successor) ? PeerId{0} : successor);
        }
        peerState.getDomain(domainId).resetOwnershipPins();
    }

    // Participation goes with None, same cacheline and same write: admission reserves this slot by
    // stamping Active without touching participation, so bits left here would let a holder grant to
    // the next occupant before it has synced. Sole writer (the dtor stopped the poll thread), so
    // write through with no rmb. The demand line went earlier, in SuccessorPolicy::leave().
    CME_FAILPOINT_REACH(failpoint::Boundary::LeaveBeforeNone);

    auto& selfState = peerState.getSelfMemberState();
    selfState.storeParticipatingDomains({});
    selfState.setStatus(Geometry::Member_t::Status::None);
    peerState.publishSelfMemberState();
}

JoinResult joinDomain(LocalPeerState& peerState, DomainId domainId)
{
    if (peerState.isParticipating(domainId))
    {
        return JoinResult::Ok;  // idempotent
    }

    // Caller holds control lock; Active check is authoritative (§1.8 closed by
    // control serialisation). Refuse if deleted before we took control.
    const auto domainRecord = peerState.loadDomainRecordSnapshot(domainId);  // get (rmb)
    if (!domainRecord.hasState(Geometry::DomainRecord_t::State::Active))
    {
        return JoinResult::UnknownDomain;
    }

    // Sync DRAM view BEFORE advertising participation (no forward to stale view).
    if (!syncDomainView(peerState, domainId))
    {
        return JoinResult::CorruptRegion;
    }

    peerState.setParticipation(domainId);

    return JoinResult::Ok;
}

LeaveResult leaveDomain(LocalPeerState& peerState, DomainId domainId)
{
    if (!peerState.isParticipating(domainId))
    {
        return LeaveResult::NotParticipating;
    }

    // Sole participant -> refuse (would orphan; deleteDomain is the only exit).
    const PeerId successor = getSuccessorOnLeave(peerState, domainId);
    if (isNoPeer(successor))
    {
        return LeaveResult::SoleParticipant;
    }

    // Hand off if held (keep a definite holder), then retract participation.
    const PeerId selfPeerId = peerState.getPeerId();
    const auto domainRecord = peerState.loadDomainRecordSnapshot(domainId);  // get (rmb)

    if (domainRecord.getHolder() == selfPeerId)
    {
        ownership_transfer::transferOwnership(peerState, domainId, successor);
    }
    peerState.getDomain(domainId).resetOwnershipPins();
    peerState.clearParticipation(domainId);

    return LeaveResult::Ok;
}

CreateResult createDomainLocked(LocalPeerState& peerState, std::string_view name,
                                DomainId& outDomainId, std::uint64_t& outIncarnation)
{
    outIncarnation = 0;

    if (name.empty() || name.size() >= Geometry::DomainRecord_t::MaxNameLen)
    {
        return CreateResult::CorruptRegion;  // API layer validates; defensive
    }

    // Control lock holds the Active set stable for uniqueness + slot scan.
    // The incarnation is discarded: the question is whether the name is taken, not by which domain.
    std::uint64_t takenIncarnation = 0;
    if (!isNoDomain(findDataDomainByName(peerState, name, takenIncarnation)))
    {
        return CreateResult::DuplicateName;
    }

    const DomainId domainId = findEmptyDataDomain(peerState);
    if (isNoDomain(domainId))
    {
        return CreateResult::NoFreeSlot;
    }

    auto record = peerState.loadDomainRecordSnapshot(domainId);  // get (rmb)
    if (!record.isValidMagic())
    {
        return CreateResult::CorruptRegion;
    }
    // Build the new incarnation in the snapshot. epoch is monotonic across incarnations
    // (NOT reset): prior holders still distinguish a newer transfer.
    record.generation = record.generation + 1;  // reuse ABA guard

    // From the snapshot this create writes, not a later read of the slot: a second visit could find
    // the incarnation of whatever took the slot after this one.
    outIncarnation = record.generation;
    std::memset(record.name, 0, Geometry::DomainRecord_t::MaxNameLen);
    std::memcpy(record.name, name.data(), name.size());
    record.holder = peerState.getPeerId();  // genesis holder = creator
    record.setState(Geometry::DomainRecord_t::State::Active);
    // Publish shadow + truth together (creator becomes holder; invariant by construction).
    ownership_transfer::publishDomainRecord(peerState, domainId, record);

    CME_FAILPOINT_REACH(failpoint::Boundary::CreateBeforeActivate);

    peerState.setActiveDomain(domainId);  // publish to the domain scan-scope bitmap

    // Record names us; adopt local holder belief (magic already checked, can't fail).
    static_cast<void>(syncDomainView(peerState, domainId));

    // Creator joins its own domain now (always >=1 participant after create).
    peerState.setParticipation(domainId);

    outDomainId = domainId;
    return CreateResult::Ok;
}

DeleteResult deleteDomainLocked(LocalPeerState& peerState, DomainId domainId)
{
    const PeerId selfPeerId = peerState.getPeerId();
    auto* record = peerState.getDomainRecord(domainId);                    // live slot for set(); get() barriers
    auto snapshot = coherency::get(record, peerState.getCoherencyMode());  // one 64B read serves checks + write base
    if (!snapshot.isValidMagic())
    {
        return DeleteResult::CorruptRegion;
    }

    if (!snapshot.hasState(Geometry::DomainRecord_t::State::Active))
    {
        return DeleteResult::Ok;  // already Free -- idempotent
    }

    // The record must name us (normal dual-lock delete) OR be vacated (holder=NoPeer:
    // an orphan reclaim, e.g. a Peterson domain the dead peer left unowned). A live
    // foreign holder is rejected; the sole-participant re-check below guards the rest.
    if (!snapshot.isHeldBy(selfPeerId) && snapshot.getHolder() != NoPeer)
    {
        return DeleteResult::NotHolder;
    }

    // Re-check participants ⊆ {self} under control lock (§1.4).
    if (hasOtherParticipant(peerState, domainId))
    {
        return DeleteResult::NotSoleParticipant;
    }

    // Retract participation before freeing the record: a crash between leaves an
    // orphan (recovery reclaims), not a stale bit on the reused slot.  Mirrors leaveDomain.
    peerState.clearParticipation(domainId);

    CME_FAILPOINT_REACH(failpoint::Boundary::DeleteBeforeFree);

    // One 64B store, so a crash lands on Active (safe) or Free, never between. holder=NoPeer
    // makes a Guard release inert. We hold control and are sole holder, so no writer races it.
    snapshot.holder = NoPeer;
    snapshot.setState(Geometry::DomainRecord_t::State::Free);
    std::memset(snapshot.name, 0, Geometry::DomainRecord_t::MaxNameLen);
    coherency::set(record, snapshot, peerState.getCoherencyMode());

    CME_FAILPOINT_REACH(failpoint::Boundary::DeleteBeforeDeactivate);

    peerState.clearActiveDomain(domainId);  // retract from the domain scan-scope bitmap

    // Drop local holder belief.
    auto& domain = peerState.getDomain(domainId);
    domain.resetOwnershipPins();
    domain.loseHolder();

    return DeleteResult::Ok;
}

std::uint32_t reclaimOrphansLocked(LocalPeerState& peerState)
{
    std::uint32_t freed = 0;

    for (const DomainId domainId : peerState.getDataDomainIdRange())
    {
        // Skip only domains WE participate in: deleteDomainLocked's sole-participant check
        // excludes self, so an idle-but-live self would otherwise sweep its own domain.
        if (peerState.isParticipating(domainId))
        {
            continue;
        }

        // Holder-agnostic: a vacated orphan (holder=NoPeer, e.g. Peterson) is still an
        // orphan. deleteDomainLocked re-checks holder (self|NoPeer) + sole-participant
        // under the control lock, so a live holder/participant is rejected there.
        const auto record = peerState.loadDomainRecordSnapshot(domainId);  // get (rmb)
        if (record.isValidMagic() &&
            record.hasState(Geometry::DomainRecord_t::State::Active) &&
            deleteDomainLocked(peerState, domainId) == DeleteResult::Ok)
        {
            ++freed;
            CME_FAILPOINT_REACH(failpoint::Boundary::ReclaimMidLoop);
        }
    }

    return freed;
}

}  // namespace cme::lifecycle
