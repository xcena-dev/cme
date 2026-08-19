// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// peer.cpp -- Peer + PeerGuard implementation.
//
// Ctor = create + join; dtor = stop poll thread + leave + deactivate.
// Hot path (lock/unlock) delegates to SuccessorPolicy; recovery runs
// independently on the poll-thread monitor (serviceRecovery).

#include "core/algo/peer.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "config.hpp"
#include "core/algo/lifecycle.hpp"
#include "core/algo/ownership_transfer.hpp"
#include "core/algo/recovery.hpp"
#include "core/layout/geometry.hpp"
#include "core/policy/liveness.hpp"
#include "core/policy/recovery_authority.hpp"
#include "core/policy/successor.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/failpoint.hpp"
#include "observe/latency.hpp"
#include "observe/stats.hpp"

namespace cme
{

namespace
{

// Poll-thread loop: per tick, heartbeat + transfer drive + recovery FSM.
void pollThreadLoop(Peer* peer, LocalPeerState* state)
{
    const auto pollInterval = state->getPollInterval();

    while (!state->isPollStopRequested())
    {
        std::this_thread::sleep_for(pollInterval);

        // Frozen/unjoined/inactive: skip tick; reset the claim latch and the takeover span so
        // recovery re-arms cleanly (recoveryAuthority is wired up alongside successorPolicy).
        if (state->isFrozen() || !state->getSuccessorPolicy() || !state->isActive())
        {
            state->getRecoveryAuthorityPolicy()->resetClaim(*state);
            state->resetRecoveryCycles();
            continue;
        }

        // Heartbeat FIRST each tick. A watcher's failure detector reads a non-advancing
        // witness as a stall, so anything that delays the bump risks false suspicion.
        ownership_transfer::bumpHeartbeat(*state);

        // One gate per tick: on a scan cycle refresh the cached scan scopes, keeping the
        // admission and Header lines cold. pollCycle reads the same latch.
        if (state->tickPeriodicScan())
        {
            state->refreshPeerScanBound();
            state->refreshActiveDomains();
        }

        state->getSuccessorPolicy()->pollCycle(*state);

        // RA monitor (oracle C): O(1)/tick, watches only this node's RA target.
        recovery::serviceRecovery(*state);

        // Orphan sweep armed by recovery FINISH; control serialises free vs create/join.
        // SweepControlTimeout rather than a 0ns probe -- see config.hpp. Retries next tick.
        if (state->isOrphanSweepPending())
        {
            auto controlGuard = peer->tryLock(ControlDomainId, SweepControlTimeout);
            // The control lock alone does not exclude this peer's own app threads, so the
            // section mutex does. Non-blocking here: a busy section retries next tick.
            const auto sectionGuard = state->tryLockControlSection();
            if (controlGuard && sectionGuard.owns_lock())
            {
                lifecycle::reclaimOrphansLocked(*state);
                state->clearOrphanSweep();
            }
        }
    }
}

// Validate joined + domainId in range; throws JoinError on failure.
// (tryLock checks separately.)
LocalPeerState& validatedState(LocalPeerState* impl, DomainId domainId, const char* opName)
{
    if (!impl || !impl->getSuccessorPolicy())
    {
        throw JoinError{std::string{"cme::Peer::"} + opName + ": peer not joined"};
    }
    if (domainId >= impl->getNumDomains())
    {
        throw JoinError{std::string{"cme::Peer::"} + opName + ": domainId out of range"};
    }
    return *impl;
}

// Refuse a slot whose incarnation differs from the one the caller resolved. Call it holding the lock that
// freezes the slot, since create and delete both take that lock.
void refuseReplacedDomain(const LocalPeerState& state, DomainId domainId,
                          std::optional<std::uint64_t> expectedIncarnation, const char* opName)
{
    if (!expectedIncarnation)
    {
        return;
    }
    if (lifecycle::readDomainIncarnation(state, domainId) != *expectedIncarnation)
    {
        throw UnknownDomainError{std::string{"cme::Peer::"} + opName +
                                 ": the domain this named is gone"};
    }
}

}  // namespace

Peer::Peer(Geometry& geometry, PeerId peerId, CoherencyMode coherency)
    : impl_{std::make_unique<LocalPeerState>()}
{
    auto& state = *impl_;

    auto* header = geometry.getHeader();
    const auto successorPolicyKind = header->getStrategy();
    // Poll cadence is not region state; seed it from the config.hpp constant.
    state.setConfig(header->numDomains, header->maxPeers, DefaultPollInterval, successorPolicyKind,
                    header->getAggregatorGroups());

    if (!state.isValidPeer(peerId))
    {
        throw JoinError{"cme::Peer: peer_id out of range"};
    }
    state.setPeerId(peerId);

    state.setRegionPointers(geometry, coherency);
    state.refreshPeerScanBound();  // seed the peerScanBound cache before any peer scan
    state.refreshActiveDomains();  // seed the activeDomains cache before any domain scan

    state.allocateDomains(state.getNumDomains());

    state.setSuccessorPolicy(makeSuccessorPolicy(successorPolicyKind));
    if (!state.getSuccessorPolicy())
    {
        throw JoinError{"cme::Peer: unknown strategy in header"};
    }
    // Build policy-private state here, single-threaded: the poll thread below and the worker
    // would otherwise race to lazy-init it.
    state.getSuccessorPolicy()->bind(state);
    state.setLivenessPolicy(makeLivenessPolicy());
    state.setRecoveryAuthorityPolicy(makeRecoveryAuthorityPolicy());

    state.activate();

    const auto joinResult = lifecycle::joinMembership(state);
    if (joinResult != lifecycle::JoinResult::Ok)
    {
        state.deactivate();
        throw JoinError{"cme::Peer: join failed"};
    }

    state.spawnPollThread(std::thread{pollThreadLoop, this, &state});
}

Peer::~Peer()
{
    if (!impl_)
    {
        return;
    }
    auto& state = *impl_;

    // Drain before leaving: Leaving drops us from successor selection, and the poll thread
    // stays up so a grant landing anyway is forwarded, not stranded on a slot about to be None.
    state.getSelfMemberState().setStatus(Geometry::Member_t::Status::Leaving);
    state.publishSelfMemberState();
    std::this_thread::sleep_for(LeaveDrainWindow);

    state.requestPollStop();
    state.joinPollThread();

    CME_FAILPOINT_REACH(failpoint::Boundary::LeaveBeforeHandoff);

    if (state.getSuccessorPolicy())
    {
        // Hand off any region role (e.g. aggregator duty) while still active, then leave.
        state.getSuccessorPolicy()->leave(state);
        lifecycle::leaveMembership(state);
    }
    state.deactivate();
}

Peer::Peer(Peer&&) noexcept = default;
Peer& Peer::operator=(Peer&&) noexcept = default;

PeerGuard Peer::lock(DomainId domainId)
{
    OBSERVE_LATENCY_BEGIN(Acquire);  // outer span: Fence + policy lock; inner stages nest
    OBSERVE_LATENCY_BEGIN(Fence);
    auto& state = validatedState(impl_.get(), domainId, "lock");
    OBSERVE_LATENCY_END(Fence, state, domainId);
    if (!state.isParticipating(domainId))
    {
        throw NotParticipatingError{"cme::Peer::lock: domain not joined"};
    }
    const timing::Deadline budget{AcquireTimeout};
    if (state.getSuccessorPolicy()->lock(state, domainId, budget) != OwnershipResult::Arrived)
    {
        throw LockTimeoutError{};
    }
    OBSERVE_LATENCY_END(Acquire, state, domainId);
    return PeerGuard{this, domainId};
}

std::optional<PeerGuard> Peer::tryLock(DomainId domainId, timing::Nanos timeout)
{
    auto& state = validatedState(impl_.get(), domainId, "tryLock");
    if (!state.isParticipating(domainId))
    {
        return std::nullopt;
    }
    // The budget starts here, at the boundary the caller asked in, so everything below spends the
    // same one. Deriving it lower would restart it and hand back more time than was asked for.
    const timing::Deadline budget{timeout};
    if (impl_->getSuccessorPolicy()->lock(state, domainId, budget) != OwnershipResult::Arrived)
    {
        return std::nullopt;
    }
    return PeerGuard{this, domainId};
}

void Peer::joinDomain(DomainId domainId, std::optional<std::uint64_t> expectedIncarnation)
{
    auto& state = validatedState(impl_.get(), domainId, "joinDomain");
    if (state.isParticipating(domainId))
    {
        // Refused rather than reported idempotent: this peer is in some domain on that slot, and a
        // caller whose incarnation disagrees is asking about a different one.
        refuseReplacedDomain(state, domainId, expectedIncarnation, "joinDomain");
        return;  // idempotent -- no control lock needed
    }

    // Control lock serialises participation publish against deleteDomain re-check.
    auto controlGuard = lock(ControlDomainId);
    const auto sectionGuard = state.lockControlSection();  // control lock is per-peer, not per-thread
    refuseReplacedDomain(state, domainId, expectedIncarnation, "joinDomain");
    const auto result = lifecycle::joinDomain(state, domainId);
    controlGuard.release();

    switch (result)
    {
        case lifecycle::JoinResult::Ok:
            return;
        case lifecycle::JoinResult::UnknownDomain:
            throw UnknownDomainError{"cme::Peer::joinDomain: domain deleted"};
        case lifecycle::JoinResult::CorruptRegion:
            throw JoinError{"cme::Peer::joinDomain: corrupt region"};
    }
}

void Peer::leaveDomain(DomainId domainId, std::optional<std::uint64_t> expectedIncarnation)
{
    auto& state = validatedState(impl_.get(), domainId, "leaveDomain");
    if (isControlDomain(domainId))
    {
        // Control underpins create/delete; mandatory.
        throw NotParticipatingError{"cme::Peer::leaveDomain: control domain is mandatory"};
    }
    // Control lock: successor scan + sole-participant guard + clear serialised against
    // concurrent leave/delete (two bare-leaves would both retract, orphaning the domain).
    auto controlGuard = lock(ControlDomainId);
    const auto sectionGuard = state.lockControlSection();
    refuseReplacedDomain(state, domainId, expectedIncarnation, "leaveDomain");
    const auto result = lifecycle::leaveDomain(state, domainId);
    controlGuard.release();

    switch (result)
    {
        case lifecycle::LeaveResult::Ok:
        case lifecycle::LeaveResult::NotParticipating:  // idempotent: already out
            return;
        case lifecycle::LeaveResult::SoleParticipant:
            throw NotParticipatingError{
                "cme::Peer::leaveDomain: sole participant; deleteDomain instead"};
    }
}

DomainId Peer::createDomain(std::string_view name)
{
    if (!impl_ || !impl_->getSuccessorPolicy())
    {
        throw JoinError{"cme::Peer::createDomain: peer not joined"};
    }

    if (name.empty() || name.size() >= Geometry::DomainRecord_t::MaxNameLen)
    {
        throw FormatError{"cme::Peer::createDomain: name length out of range"};
    }

    // Control lock serialises registry claim/publish against other create/delete.
    auto controlGuard = lock(ControlDomainId);
    const auto sectionGuard = impl_->lockControlSection();
    DomainId newDomainId = 0;
    const auto result = lifecycle::createDomainLocked(*impl_, name, newDomainId);
    controlGuard.release();

    switch (result)
    {
        case lifecycle::CreateResult::Ok:
            return newDomainId;
        case lifecycle::CreateResult::DuplicateName:
            throw DomainExistsError{std::string{"cme: domain exists: "} + std::string{name}};
        case lifecycle::CreateResult::NoFreeSlot:
            throw DomainLimitError{"cme: domain slot ceiling reached"};
        case lifecycle::CreateResult::CorruptRegion:
            break;
    }

    throw JoinError{"cme::Peer::createDomain: corrupt region"};
}

void Peer::deleteDomain(DomainId domainId, std::optional<std::uint64_t> expectedIncarnation)
{
    auto& state = validatedState(impl_.get(), domainId, "deleteDomain");
    if (isControlDomain(domainId))
    {
        throw JoinError{"cme::Peer::deleteDomain: control domain is not deletable"};
    }

    // Dual-lock (§1.4): target domain first, THEN control -- no inversion with
    // createDomain (control-only), so no deadlock.
    auto domainGuard = lock(domainId);
    auto controlGuard = lock(ControlDomainId);
    const auto sectionGuard = state.lockControlSection();  // always last: no inversion
    // Both locks held, so this is the last word: nothing can replace the domain between here and the flip
    // to Free, and deleting the wrong domain is unrecoverable.
    refuseReplacedDomain(state, domainId, expectedIncarnation, "deleteDomain");
    const auto result = lifecycle::deleteDomainLocked(state, domainId);
    controlGuard.release();
    domainGuard.release();

    switch (result)
    {
        case lifecycle::DeleteResult::Ok:
            return;
        case lifecycle::DeleteResult::NotSoleParticipant:
            throw NotParticipatingError{
                "cme::Peer::deleteDomain: other participants remain; they must leaveDomain first"};
        case lifecycle::DeleteResult::NotHolder:
            throw JoinError{"cme::Peer::deleteDomain: not holder"};
        case lifecycle::DeleteResult::CorruptRegion:
            throw JoinError{"cme::Peer::deleteDomain: corrupt region"};
    }
}

void Peer::setFreeze(bool frozen) noexcept
{
    if (impl_)
    {
        impl_->setFreeze(frozen);
    }
}

DomainId Peer::resolveDomainName(std::string_view name, std::uint64_t& outIncarnation) const
{
    if (!impl_)
    {
        outIncarnation = 0;
        return NoDomain;
    }
    return lifecycle::findDataDomainByName(*impl_, name, outIncarnation);
}

std::uint64_t Peer::readDomainIncarnation(DomainId domainId) const
{
    // Range-checked here because this is where an id from outside the library enters: a handle carries
    // one the caller may have built rather than resolved, and getDomainRecord is bare index arithmetic.
    if (!impl_ || domainId >= impl_->getNumDomains())
    {
        return 0;  // not a slot at all, and no live incarnation is zero
    }
    return lifecycle::readDomainIncarnation(*impl_, domainId);
}

CoherencyMode Peer::getCoherencyMode() const noexcept
{
    return impl_ ? impl_->getCoherencyMode() : CoherencyMode::Flush;
}

PeerId Peer::getPeerId() const noexcept
{
    return impl_ ? impl_->getPeerId() : PeerId{0};
}

TelemetrySnapshot_t Peer::getTelemetry() const
{
    return impl_ ? stats::getTelemetrySnapshot(*impl_) : TelemetrySnapshot_t{};
}

LocalPeerState& Peer::getPeerState() noexcept
{
    return *impl_;
}

void Peer::releaseInternal(DomainId domainId) noexcept
{
    if (impl_ && impl_->getSuccessorPolicy())
    {
        impl_->getSuccessorPolicy()->unlock(*impl_.get(), domainId);
    }
}

// ── PeerGuard ──────────────────────────────────────────────────────────

PeerGuard::PeerGuard(PeerGuard&& other) noexcept
    : peer_{other.peer_},
      domainId_{other.domainId_}
{
    other.peer_ = nullptr;
}

PeerGuard& PeerGuard::operator=(PeerGuard&& other) noexcept
{
    if (this != &other)
    {
        release();
        peer_ = other.peer_;
        domainId_ = other.domainId_;
        other.peer_ = nullptr;
    }
    return *this;
}

PeerGuard::~PeerGuard() noexcept
{
    release();
}

void PeerGuard::release() noexcept
{
    if (peer_ != nullptr)
    {
        peer_->releaseInternal(domainId_);
        peer_ = nullptr;
    }
}

}  // namespace cme
