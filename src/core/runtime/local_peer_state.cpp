// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// local_peer_state.cpp -- LocalPeerState method implementations.

#include "core/runtime/local_peer_state.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#include "cme/shared.hpp"
#include "config.hpp"
#include "core/domain_bitmap.hpp"
#include "core/layout/geometry.hpp"
#include "core/layout/geometry_profile.hpp"
#include "core/policy/liveness.hpp"
#include "core/policy/recovery_authority.hpp"
#include "core/policy/successor_policy.hpp"
#include "core/runtime/local_domain_view.hpp"
#include "core/types.hpp"
#include "observe/telemetry.hpp"
#include "util/coherency.hpp"
#include "util/time.hpp"

namespace cme
{

PeerId LocalPeerState::getPeerId() const noexcept
{
    return peerId_;
}
void LocalPeerState::setPeerId(PeerId peerId) noexcept
{
    peerId_ = peerId;
}

void LocalPeerState::activate() noexcept
{
    active_.store(true, std::memory_order_release);
}

void LocalPeerState::stampSelfSeen() noexcept
{
    const std::uint64_t now = time::clockNowNanos();
    // Monotone-published guard: never publish a backward wall-clock step -- hold the
    // prior stamp so a step becomes a stall (the safe, conservative direction).
    if (now > static_cast<std::uint64_t>(selfMember_.lastSeenNanos))
    {
        selfMember_.lastSeenNanos = now;
    }
}

void LocalPeerState::seedSelfMemberState(const Geometry::Member_t& member) noexcept
{
    selfMember_ = member;
}

Geometry::Member_t& LocalPeerState::getSelfMemberState() noexcept
{
    return selfMember_;
}

void LocalPeerState::publishSelfMemberState() noexcept
{
    // Write-through the local truth; self owns its slot so no rmb (whole-slot set).
    coherency::set(getMemberSlot(getPeerId()), selfMember_, getCoherencyMode());
}

void LocalPeerState::deactivate() noexcept
{
    active_.store(false, std::memory_order_release);
}

bool LocalPeerState::isActive() const noexcept
{
    return active_.load(std::memory_order_acquire);
}

void LocalPeerState::setRegionPointers(const Geometry& geometry, CoherencyMode mode) noexcept
{
    region_.coherencyMode = mode;
    region_.header = geometry.getHeader();
    region_.admissionControl = geometry.getAdmissionControl();
    region_.domainRecords = geometry.getDomainRecord(0);
    region_.members = geometry.getMemberSlot(0);
    region_.recoveryAuthorityArea = geometry.getRecoveryAuthorityAreaBase();
    region_.successorArea = geometry.getSuccessorAreaBase();
    region_.profiles = geometry.getProfileSlot(0);
}

Geometry::DomainRecord_t* LocalPeerState::getDomainRecord(DomainId domainId) const noexcept
{
    // Records are replicated getRecordsPerDomain apart (truth + per-group shadows); the
    // truth copy sits at the block head, so stride by the full block. Pointer only --
    // a fresh read is the caller's coherency::get().
    const std::uint64_t stride = Geometry::getRecordsPerDomain(config_.maxPeers);
    return &region_.domainRecords[static_cast<std::uint64_t>(domainId) * stride];
}

Geometry::DomainRecord_t* LocalPeerState::getDomainRecordShadow(DomainId domainId, PeerId peerId) const noexcept
{
    // Replica block: truth at the head, shadows at 1 + group (mirrors Geometry layout).
    const std::uint64_t stride = Geometry::getRecordsPerDomain(config_.maxPeers);
    const std::uint64_t index =
        static_cast<std::uint64_t>(domainId) * stride + 1 + Geometry::getGroupIndex(peerId);
    return &region_.domainRecords[index];
}

std::uint32_t LocalPeerState::getShadowGroupCount() const noexcept
{
    return Geometry::getGroupCount(config_.maxPeers);
}

Geometry::Member_t* LocalPeerState::getMemberSlot(PeerId peerId) const noexcept
{
    return &region_.members[peerId];
}

std::uint8_t* LocalPeerState::getRecoveryAuthorityAreaBase() const noexcept
{
    return region_.recoveryAuthorityArea;
}

MemberProfile_t* LocalPeerState::getProfileSlot(PeerId peerId) const noexcept
{
    // profiles may be nullptr when the section is absent.
    return region_.profiles ? &region_.profiles[peerId] : nullptr;
}

std::uint8_t* LocalPeerState::getSuccessorAreaBase() const noexcept
{
    return region_.successorArea;
}

// 64B bulk snapshots: rmb + one wide load (coherency::get) for read-only callers.
// 1 txn on UC vs per-field rmb. NOT for write-following paths -- the returned
// copy is detached from the live slot.
Geometry::DomainRecord_t LocalPeerState::loadDomainRecordSnapshot(DomainId domainId) const noexcept
{
    return coherency::get(getDomainRecord(domainId), getCoherencyMode());
}
Geometry::Member_t LocalPeerState::loadMemberSnapshot(PeerId peerId) const noexcept
{
    return coherency::get(getMemberSlot(peerId), getCoherencyMode());
}
// Caller must ensure the profile section exists (getProfileSlot returns nullptr otherwise).
MemberProfile_t LocalPeerState::loadProfileSnapshot(PeerId peerId) const noexcept
{
    return coherency::get(getProfileSlot(peerId), getCoherencyMode());
}

void LocalPeerState::setActiveDomain(DomainId domain) noexcept
{
    coherency::rmw(region_.header, getCoherencyMode(), [domain](auto* header)
                   {
                       DomainBitmap bits = header->loadActiveDomains();
                       bits.set(domain);
                       header->storeActiveDomains(bits);
                   });
}

void LocalPeerState::clearActiveDomain(DomainId domain) noexcept
{
    coherency::rmw(region_.header, getCoherencyMode(), [domain](auto* header)
                   {
                       DomainBitmap bits = header->loadActiveDomains();
                       bits.clear(domain);
                       header->storeActiveDomains(bits);
                   });
}

void LocalPeerState::setConfig(std::uint32_t numDomains, std::uint32_t maxPeers,
                               std::chrono::microseconds pollInterval, Strategy kind,
                               std::uint32_t aggregatorGroups) noexcept
{
    config_.numDomains = numDomains;
    config_.maxPeers = maxPeers;
    config_.pollInterval = pollInterval;
    config_.successorPolicyKind = kind;
    config_.aggregatorGroups = aggregatorGroups;
    memberCache_ = std::make_unique<CachedMember_t[]>(maxPeers);
}

std::uint32_t LocalPeerState::getNumDomains() const noexcept
{
    return config_.numDomains;
}
std::uint32_t LocalPeerState::getMaxPeers() const noexcept
{
    return config_.maxPeers;
}
std::chrono::microseconds LocalPeerState::getPollInterval() const noexcept
{
    return config_.pollInterval;
}
Strategy LocalPeerState::getSuccessorPolicyKind() const noexcept
{
    return config_.successorPolicyKind;
}
std::uint32_t LocalPeerState::getAggregatorGroups() const noexcept
{
    return config_.aggregatorGroups;
}

void LocalPeerState::setSuccessorPolicy(std::unique_ptr<SuccessorPolicy> successor) noexcept
{
    policy_.successor = std::move(successor);
}

SuccessorPolicy* LocalPeerState::getSuccessorPolicy() noexcept
{
    return policy_.successor.get();
}
const SuccessorPolicy* LocalPeerState::getSuccessorPolicy() const noexcept
{
    return policy_.successor.get();
}

void LocalPeerState::setLivenessPolicy(std::unique_ptr<LivenessPolicy> liveness) noexcept
{
    policy_.liveness = std::move(liveness);
}

LivenessPolicy* LocalPeerState::getLivenessPolicy() noexcept
{
    return policy_.liveness.get();
}
const LivenessPolicy* LocalPeerState::getLivenessPolicy() const noexcept
{
    return policy_.liveness.get();
}

void LocalPeerState::setRecoveryAuthorityPolicy(std::unique_ptr<RecoveryAuthorityPolicy> recoveryAuthority) noexcept
{
    policy_.recoveryAuthority = std::move(recoveryAuthority);
}

RecoveryAuthorityPolicy* LocalPeerState::getRecoveryAuthorityPolicy() noexcept
{
    return policy_.recoveryAuthority.get();
}
const RecoveryAuthorityPolicy* LocalPeerState::getRecoveryAuthorityPolicy() const noexcept
{
    return policy_.recoveryAuthority.get();
}

void LocalPeerState::allocateDomains(std::uint32_t count)
{
    // Participation lives in selfMember_ (seeded by joinMembership); this only allocates
    // the per-domain DRAM views.
    domains_ = std::make_unique<LocalDomainView[]>(count);
}

DomainBitmap LocalPeerState::getParticipating() const noexcept
{
    return selfMember_.loadParticipatingDomains();
}
bool LocalPeerState::isParticipating(DomainId domainId) const noexcept
{
    return selfMember_.loadParticipatingDomains().has(domainId);
}
namespace
{

// Atomic bit RMW on one participation word of selfMember_. DRAM-only by construction: the
// caller is always this peer's own DRAM copy, never a region slot -- a lock-prefixed RMW is
// not a valid primitive on FAM, which has no cross-host coherence (see coherency.hpp).
// The control lock these run under is peer-granularity, so a second thread of this peer
// passes it through the resident fast path; without the atomic, two workers joining domains
// in the same word would drop one of the bits for good.
void participationBitRmw(Geometry::Member_t& selfMember, DomainId domainId, bool set) noexcept
{
    auto* word = reinterpret_cast<std::atomic<std::uint64_t>*>(
        &selfMember.participatingDomains[domainId / DomainBitsPerWord].raw);
    const std::uint64_t mask = std::uint64_t{1} << (domainId % DomainBitsPerWord);
    if (set)
    {
        word->fetch_or(mask, std::memory_order_relaxed);
    }
    else
    {
        word->fetch_and(~mask, std::memory_order_relaxed);
    }
}

}  // namespace

// The whole-slot publish can still carry a snapshot missing a concurrent bit; the next
// publish (a worker's, or the 10 us heartbeat) carries both.
void LocalPeerState::setParticipation(DomainId domainId) noexcept
{
    participationBitRmw(selfMember_, domainId, true);
    publishSelfMemberState();
}
void LocalPeerState::clearParticipation(DomainId domainId) noexcept
{
    participationBitRmw(selfMember_, domainId, false);
    publishSelfMemberState();
}
bool LocalPeerState::isPendingDomain(DomainId domainId) const noexcept
{
    return selfPending_.has(domainId);
}

LocalDomainView& LocalPeerState::getDomain(DomainId domainId) noexcept
{
    return domains_[domainId];
}
const LocalDomainView& LocalPeerState::getDomain(DomainId domainId) const noexcept
{
    return domains_[domainId];
}

LocalPeerState::~LocalPeerState()
{
    // Safety net: ~std::thread on joinable calls std::terminate. Normal teardown
    // already stopped+joined; this covers any other drop path. Both are idempotent.
    requestPollStop();
    joinPollThread();
}

void LocalPeerState::spawnPollThread(std::thread pollThread) noexcept
{
    poll_.thread = std::move(pollThread);
}

void LocalPeerState::requestPollStop() noexcept
{
    poll_.stop.store(true, std::memory_order_release);
}

bool LocalPeerState::isPollStopRequested() const noexcept
{
    return poll_.stop.load(std::memory_order_acquire);
}

void LocalPeerState::joinPollThread() noexcept
{
    if (poll_.thread.joinable())
    {
        poll_.thread.join();
    }
}

void LocalPeerState::setFreeze(bool frozen) noexcept
{
    poll_.freeze.store(frozen, std::memory_order_release);
}

bool LocalPeerState::isFrozen() const noexcept
{
    return poll_.freeze.load(std::memory_order_acquire);
}

bool LocalPeerState::isRecoveryArmed(PeerId targetPeerId) const noexcept
{
    return poll_.recoveryCycles > 0 && poll_.recoveryTarget == targetPeerId;
}

void LocalPeerState::setRecoveryCycles(PeerId targetPeerId) noexcept
{
    poll_.recoveryCycles = RecoveryCycles;
    poll_.recoveryTarget = targetPeerId;
}

void LocalPeerState::resetRecoveryCycles() noexcept
{
    poll_.recoveryCycles = 0;
    poll_.recoveryTarget = NoPeer;
}

bool LocalPeerState::isRecoveryOngoing() noexcept
{
    if (poll_.recoveryCycles == 0)
    {
        return false;
    }
    --poll_.recoveryCycles;
    if (poll_.recoveryCycles == 0)
    {
        poll_.recoveryTarget = NoPeer;
        return false;
    }
    return true;
}

void LocalPeerState::requestOrphanSweep() noexcept
{
    poll_.orphanSweepPending = true;
}

bool LocalPeerState::isOrphanSweepPending() const noexcept
{
    return poll_.orphanSweepPending;
}

void LocalPeerState::clearOrphanSweep() noexcept
{
    poll_.orphanSweepPending = false;
}

void LocalPeerState::refreshPeerScanBound() noexcept
{
    // Re-read the cached membership extent from the admission line (one cacheline).
    peerScanBound_ = coherency::get(region_.admissionControl, getCoherencyMode()).peerScanBound;  // rmb + 64B read
}

void LocalPeerState::refreshActiveDomains() noexcept
{
    // Re-read the cached domain scan scope from the Header bitmap (one cacheline).
    activeDomains_ = coherency::get(region_.header, getCoherencyMode()).loadActiveDomains();  // rmb + 64B read
}

bool LocalPeerState::tickPeriodicScan() noexcept
{
    if (poll_.periodicScanCountdown == 0)
    {
        poll_.periodicScanCountdown = PeriodicScanInterval - 1;
        return true;
    }
    --poll_.periodicScanCountdown;
    return false;
}

LocalPeerState::MemberView_t LocalPeerState::getMemberView(PeerId peerId,
                                                           std::chrono::nanoseconds maxAge) const noexcept
{
    MemberView_t view;
    if (!isValidPeer(peerId))
    {
        return view;
    }
    auto& slot = memberCache_[peerId];
    const std::int64_t nowNs = time::getMonoTime().time_since_epoch().count();
    if (slot.valid.load(std::memory_order_relaxed) &&
        nowNs - slot.stampNs.load(std::memory_order_relaxed) <= maxAge.count())
    {
        view.active = slot.status.load(std::memory_order_relaxed) ==
                      static_cast<std::uint32_t>(Geometry::Member_t::Status::Active);
        for (std::uint32_t word = 0; word < DomainWordCount; ++word)
        {
            view.participating.setWord(word, slot.participating[word].load(std::memory_order_relaxed));
        }
        view.lastSeenNanos = slot.lastSeenNanos.load(std::memory_order_relaxed);
        return view;
    }
    // Miss: refresh from FAM, fill the cache, and answer from that same read.
    const Geometry::Member_t member = loadMemberSnapshot(peerId);
    cacheMemberSnapshot(peerId, member);
    const bool magicValid = member.isValidMagic();
    view.active = magicValid && member.hasStatus(Geometry::Member_t::Status::Active);
    view.participating = magicValid ? member.loadParticipatingDomains() : DomainBitmap{};
    view.lastSeenNanos = magicValid ? static_cast<std::uint64_t>(member.lastSeenNanos) : 0;
    return view;
}

void LocalPeerState::cacheMemberSnapshot(PeerId peerId, const Geometry::Member_t& member) const noexcept
{
    if (!isValidPeer(peerId))
    {
        return;
    }
    auto& slot = memberCache_[peerId];
    const bool magicValid = member.isValidMagic();
    const DomainBitmap participating = magicValid ? member.loadParticipatingDomains() : DomainBitmap{};
    for (std::uint32_t word = 0; word < DomainWordCount; ++word)
    {
        slot.participating[word].store(participating.getWord(word), std::memory_order_relaxed);
    }
    slot.lastSeenNanos.store(magicValid ? static_cast<std::uint64_t>(member.lastSeenNanos) : 0,
                             std::memory_order_relaxed);
    slot.status.store(magicValid ? static_cast<std::uint32_t>(member.status) : 0,
                      std::memory_order_relaxed);
    slot.valid.store(magicValid, std::memory_order_relaxed);
    // Stamp last: a reader that sees this stamp must already see the fields above.
    slot.stampNs.store(time::getMonoTime().time_since_epoch().count(), std::memory_order_relaxed);
}

PeerTelemetry_t& LocalPeerState::getTelemetry() noexcept
{
    return telemetry_;
}
const PeerTelemetry_t& LocalPeerState::getTelemetry() const noexcept
{
    return telemetry_;
}

}  // namespace cme
