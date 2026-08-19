// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// local_peer_state.hpp -- per-peer DRAM-side runtime state.
//
// Complements the SWPC line tables in shared memory: holds region pointers,
// per-domain ownership state, policy oracles, poll thread, and diagnostics.
// Local-only state that must not sit in shared memory.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "cme/shared.hpp"
#include "common/timing.hpp"
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

namespace cme
{

// One instance per attached peer; lives only in this process's DRAM.
class LocalPeerState
{
public:
    // ── identity + lifecycle ─────────────────────────────────────
    ~LocalPeerState();  // safety net: stops + joins the poll thread
    [[nodiscard]] PeerId getPeerId() const noexcept;
    void setPeerId(PeerId peerId) noexcept;
    void activate() noexcept;
    void deactivate() noexcept;
    [[nodiscard]] bool isActive() const noexcept;
    // Stamp self's liveness witness with the current wall clock; the FAM slot is a
    // write-only mirror of this DRAM truth (self owns its slot, no FAM read-back).
    void stampSelfSeen() noexcept;
    // Local truth of self's member slot, seeded once by joinMembership's get (the only
    // self FAM read). Mutate getSelfMemberState(), then publishSelfMemberState() to
    // write-through to FAM (whole-slot set); never read the FAM slot back.
    void seedSelfMemberState(const Geometry::Member_t& member) noexcept;
    [[nodiscard]] Geometry::Member_t& getSelfMemberState() noexcept;
    void publishSelfMemberState() noexcept;

    // ── SWPC line table pointers (set once at attach) ────────────
    // Capture every region section pointer (header, admission line, slot tables) from
    // the bound geometry in one shot.
    // @mode is the Session's, from FormatOpts_t / OpenOpts_t -- it is not derivable from the
    // geometry, since it describes how this peer reaches the region rather than the region.
    void setRegionPointers(const Geometry& geometry, CoherencyMode mode) noexcept;
    // Slot accessors return a pointer only; a fresh read is the caller's
    // coherency::get(...), an in-place update its rmw/rmwIfTrue (those barrier themselves).
    [[nodiscard]] Geometry::DomainRecord_t* getDomainRecord(DomainId domainId) const noexcept;
    // Shadow replica of the domain record for peerId's group (fast USE-detection poll target).
    [[nodiscard]] Geometry::DomainRecord_t* getDomainRecordShadow(DomainId domainId, PeerId peerId) const noexcept;
    // Number of shadow groups: lets a caller sweep every shadow (recovery frontier scan)
    // via getDomainRecordShadow on one peer per group.
    [[nodiscard]] std::uint32_t getShadowGroupCount() const noexcept;
    [[nodiscard]] Geometry::Member_t* getMemberSlot(PeerId peerId) const noexcept;
    [[nodiscard]] MemberProfile_t* getProfileSlot(PeerId peerId) const noexcept;
    [[nodiscard]] std::uint8_t* getSuccessorAreaBase() const noexcept;
    // Base of the RA-policy-private claim region; the RA policy casts it to its own slot.
    [[nodiscard]] std::uint8_t* getRecoveryAuthorityAreaBase() const noexcept;
    // Captured with the pointers above, from the Memory this peer mapped. Every
    // coherency::get/set/rmw on a region slot takes it.
    [[nodiscard]] coherency::Mode getCoherencyMode() const noexcept
    {
        return region_.coherencyMode;
    }

    // 64B bulk snapshots: rmb + one wide load (coherency::get). For read-only callers
    // (scans/checks) -- 1 txn on UC vs per-field rmb. NOT for write-following paths
    // (the returned copy is not the live slot).
    [[nodiscard]] Geometry::DomainRecord_t loadDomainRecordSnapshot(DomainId domainId) const noexcept;
    [[nodiscard]] Geometry::Member_t loadMemberSnapshot(PeerId peerId) const noexcept;
    [[nodiscard]] MemberProfile_t loadProfileSnapshot(PeerId peerId) const noexcept;

    // ── member cache (lazy read-through) ─────────────────────────
    // A peer's cached member fields, returned by getMemberView.
    struct MemberView_t
    {
        bool active{false};
        DomainBitmap participating;
        std::uint64_t lastSeenNanos{0};
    };
    // Cached view if refreshed within @maxAge, else one FAM read and a re-stamp; maxAge==0
    // forces the read. Fields are relaxed atomic words, so a concurrent refresh cannot tear.
    [[nodiscard]] MemberView_t getMemberView(PeerId peerId, timing::Nanos maxAge) const noexcept;
    // Fill the cache from a slot the caller already read, so a scan that reads for its own
    // reasons (the RA ring walk) pays the FAM read once and every later getMemberView hits.
    void cacheMemberSnapshot(PeerId peerId, const Geometry::Member_t& member) const noexcept;

    // ── config (immutable after attach) ──────────────────────────
    void setConfig(std::uint32_t numDomains, std::uint32_t maxPeers,
                   timing::Nanos pollInterval, Strategy kind,
                   std::uint32_t aggregatorGroups) noexcept;
    [[nodiscard]] std::uint32_t getNumDomains() const noexcept;
    [[nodiscard]] std::uint32_t getMaxPeers() const noexcept;
    [[nodiscard]] timing::Nanos getPollInterval() const noexcept;
    [[nodiscard]] Strategy getSuccessorPolicyKind() const noexcept;
    [[nodiscard]] std::uint32_t getAggregatorGroups() const noexcept;

    // True if peerId names a provisioned slot. NoPeer (UINT32_MAX) fails the
    // unsigned compare. Gate peer-keyed table indexing on this.
    [[nodiscard]] bool isValidPeer(PeerId peerId) const noexcept
    {
        return peerId < getMaxPeers();
    }

    // Range-based-for over ids: `for (DomainId d : getDomainIdRange())`.
    // getDataDomainIdRange() skips the control domain (slot 0). All zero-overhead.
    [[nodiscard]] IdRange<DomainId> getDomainIdRange() const noexcept
    {
        return {DomainId{0}, getNumDomains()};
    }
    [[nodiscard]] IdRange<DomainId> getDataDomainIdRange() const noexcept
    {
        return {DomainId{1}, getNumDomains()};
    }
    // Flip a domain's live bit in the Header activeDomains bitmap (the domain scan
    // scope). Control-lock writer: create sets, delete clears -- single writer at a
    // time, so a plain read-modify-write is safe.
    void setActiveDomain(DomainId domain) noexcept;
    void clearActiveDomain(DomainId domain) noexcept;
    // Live domains to scan, from the DRAM-cached activeDomains bitmap (refreshActiveDomains):
    // range-for yields only set bits, skipping Free slots. getActiveDataDomainRange drops the
    // control domain. A stale bit is harmless -- the loop body re-validates each record.
    [[nodiscard]] DomainBitmap getActiveDomainRange() const noexcept
    {
        return activeDomains_;
    }
    [[nodiscard]] DomainBitmap getActiveDataDomainRange() const noexcept
    {
        DomainBitmap data = activeDomains_;
        data.clear(ControlDomainId);
        return data;
    }
    // Peer-slot ranges bounded by peerScanBound, the live membership extent. A stale-low bound
    // only defers the newest joiner; the maxPeers fallback is safe and keeps the modulus nonzero.
    [[nodiscard]] std::uint32_t getPeerScanBound() const noexcept
    {
        return peerScanBound_ != 0 ? peerScanBound_ : getMaxPeers();
    }
    [[nodiscard]] IdRange<PeerId> getPeerIdRange() const noexcept
    {
        return {PeerId{0}, getPeerScanBound()};
    }
    [[nodiscard]] IdRing<PeerId> getPeerRing() const noexcept
    {
        return {peerId_, getPeerScanBound()};
    }

    // ── policy oracles (SWOT Layer 0) + per-domain state ─────────
    void setSuccessorPolicy(std::unique_ptr<SuccessorPolicy> successor) noexcept;
    [[nodiscard]] SuccessorPolicy* getSuccessorPolicy() noexcept;
    [[nodiscard]] const SuccessorPolicy* getSuccessorPolicy() const noexcept;
    void setLivenessPolicy(std::unique_ptr<LivenessPolicy> liveness) noexcept;
    [[nodiscard]] LivenessPolicy* getLivenessPolicy() noexcept;
    [[nodiscard]] const LivenessPolicy* getLivenessPolicy() const noexcept;
    void setRecoveryAuthorityPolicy(std::unique_ptr<RecoveryAuthorityPolicy> recoveryAuthority) noexcept;
    [[nodiscard]] RecoveryAuthorityPolicy* getRecoveryAuthorityPolicy() noexcept;
    [[nodiscard]] const RecoveryAuthorityPolicy* getRecoveryAuthorityPolicy() const noexcept;
    void allocateDomains(std::uint32_t count);
    [[nodiscard]] LocalDomainView& getDomain(DomainId domainId) noexcept;
    [[nodiscard]] const LocalDomainView& getDomain(DomainId domainId) const noexcept;

    // ── participation intent (DRAM source of truth) ──────────────
    // Domains this peer has joined; rejoin restores the same set.
    [[nodiscard]] DomainBitmap getParticipating() const noexcept;
    [[nodiscard]] bool isParticipating(DomainId domainId) const noexcept;
    void setParticipation(DomainId domainId) noexcept;
    void clearParticipation(DomainId domainId) noexcept;

    // DRAM self-shadow of this peer's REQUEST demand bits; the policy owns the FAM region.
    // @publish runs under the mutex, serialising the demand-line write against other workers.
    template <typename T_Publish>
    void updateSelfPending(DomainId domainId, bool requesting, T_Publish&& publish) noexcept
    {
        const std::lock_guard<std::mutex> guard(pendingDomainsMutex_);
        if (requesting)
        {
            selfPending_.set(domainId);
        }
        else
        {
            selfPending_.clear(domainId);
        }
        publish(selfPending_);
    }
    // Clear every demand bit (leave/teardown), publishing the emptied line under the lock.
    template <typename T_Publish>
    void clearSelfPending(T_Publish&& publish) noexcept
    {
        const std::lock_guard<std::mutex> guard(pendingDomainsMutex_);
        selfPending_ = DomainBitmap{};
        publish(selfPending_);
    }
    // True if a worker thread is in lock() waiting on this domain (pending request).
    // Lock-free best-effort read for the poll thread; a stale bit self-corrects next cycle.
    [[nodiscard]] bool isPendingDomain(DomainId domainId) const noexcept;

    // Serialises this peer's OWN threads inside a control-lock critical section. The control
    // lock is a per-peer token, so a second thread of this peer enters it through the resident
    // fast path -- which is what let the poll thread's orphan sweep run concurrently with an app
    // thread's createDomain. Always taken AFTER the control lock: holding it across a blocking
    // acquire would stall this peer's other threads for up to AcquireTimeout.
    [[nodiscard]] std::unique_lock<std::mutex> lockControlSection() noexcept
    {
        return std::unique_lock<std::mutex>{controlSectionMutex_};
    }
    // Poll-thread variant: never blocks, so a busy section just retries next tick. Test the
    // returned lock for ownership before entering.
    [[nodiscard]] std::unique_lock<std::mutex> tryLockControlSection() noexcept
    {
        return std::unique_lock<std::mutex>{controlSectionMutex_, std::try_to_lock};
    }

    // ── poll thread ──────────────────────────────────────────────
    void spawnPollThread(std::thread pollThread) noexcept;
    void requestPollStop() noexcept;
    [[nodiscard]] bool isPollStopRequested() const noexcept;
    void joinPollThread() noexcept;
    void setFreeze(bool frozen) noexcept;
    [[nodiscard]] bool isFrozen() const noexcept;

    // Recovery takeover span (poll thread only): the RecoveryCycles countdown, armed once a
    // claim is confirmed. Peek without side effect -- "arm now" vs "already in the span".
    // Keyed to the target so a target switch cannot inherit a leftover span.
    [[nodiscard]] bool isRecoveryArmed(PeerId targetPeerId) const noexcept;
    // Arm the countdown to RecoveryCycles for @targetPeerId.
    void setRecoveryCycles(PeerId targetPeerId) noexcept;
    // Drop the span. Every abort path must call this, or the next genuine recovery starts
    // mid-span and skips arming (no participation clear, no Recovering stamp).
    void resetRecoveryCycles() noexcept;
    // Decrement countdown; returns true while cycles remain, false when elapsed
    // (-> finalise). Mutate-on-query, like tickPeriodicScan.
    [[nodiscard]] bool isRecoveryOngoing() noexcept;

    // Orphan-sweep latch (poll thread only). Set by recovery FINISH; sticky
    // until the poll loop acquires control and frees the orphan.
    void requestOrphanSweep() noexcept;
    [[nodiscard]] bool isOrphanSweepPending() const noexcept;
    void clearOrphanSweep() noexcept;

    // Scan-scope refresh gate (poll thread only). Decrements one countdown and returns
    // true once every PeriodicScanInterval cycles -- call it exactly once per tick; the
    // poll loop runs the two scan-scope refreshes on a true return. Mutate-on-query.
    bool tickPeriodicScan() noexcept;

    // Cold-line re-reads driven by the periodic-scan gate: the admission line (membership
    // extent) and the Header activeDomains bitmap. Also seed the caches once at attach.
    void refreshPeerScanBound() noexcept;
    void refreshActiveDomains() noexcept;

    // ── diagnostics ──────────────────────────────────────────────
    [[nodiscard]] PeerTelemetry_t& getTelemetry() noexcept;
    [[nodiscard]] const PeerTelemetry_t& getTelemetry() const noexcept;

private:
    PeerId peerId_{0};
    Geometry::Member_t selfMember_{};  // local truth of self's slot (incl. lastSeenNanos); FAM is a write-only mirror
    std::atomic<bool> active_{false};

    std::uint32_t peerScanBound_{0};  // cached AdmissionControl_t::peerScanBound; bounds peer scans

    // Lazy read-through DRAM cache of member fields; see getMemberView. Sized maxPeers at
    // setConfig. Each field <=8B aligned -> tear-free relaxed atomics (lock-free multi-writer).
    struct CachedMember_t
    {
        // Per-word atomics: tear-free within a word, mixed-epoch across words -- the same
        // bounded-stale tolerance the cache already relies on.
        std::atomic<std::uint64_t> participating[DomainWordCount]{};  // participatingDomains words
        std::atomic<std::uint64_t> lastSeenNanos{0};                  // LivenessPolicy witness (wall-clock ns)
        std::atomic<std::int64_t> stampNs{INT64_MIN};                 // monotime ns of last refresh (old => stale)
        std::atomic<std::uint32_t> status{0};                         // Member_t::Status
        std::atomic<bool> valid{false};                               // slot had valid magic at last refresh
    };
    std::unique_ptr<CachedMember_t[]> memberCache_;

    std::unique_ptr<LocalDomainView[]> domains_;
    DomainBitmap activeDomains_;      // cached Header activeDomains; bounds domain scans
    DomainBitmap selfPending_;        // DRAM shadow of this peer's REQUEST demand bits (poll-read via isPendingDomain)
    std::mutex pendingDomainsMutex_;  // serialises this peer's worker threads' pending writes
    std::mutex controlSectionMutex_;  // serialises this peer's threads inside a control section

    struct
    {
        Geometry::Header_t* header{nullptr};  // region base (activeDomains bitmap)
        Geometry::AdmissionControl_t* admissionControl{nullptr};
        Geometry::DomainRecord_t* domainRecords{nullptr};
        Geometry::Member_t* members{nullptr};
        // policy-private regions
        std::uint8_t* recoveryAuthorityArea{nullptr};  // RA-policy-private claim region base
        std::uint8_t* successorArea{nullptr};          // strategy tail area base; nullptr if none
        // independent optional feature (CME_PROFILE), last
        MemberProfile_t* profiles{nullptr};
        // Not a pointer, but captured from the same Geometry in the same shot: the mapping's
        // coherency regime. Defaulted to the strictest so an unset state cannot skip a flush.
        coherency::Mode coherencyMode{coherency::Mode::Flush};
    } region_;

    struct
    {
        std::uint32_t numDomains{0};
        std::uint32_t maxPeers{0};
        timing::Nanos pollInterval{DefaultPollInterval};
        Strategy successorPolicyKind{Strategy::Order};
        std::uint32_t aggregatorGroups{0};  // RequestAgg group count (0 = auto)
    } config_;

    // SWOT Layer-0 policy oracles (A / B / C).
    struct
    {
        std::unique_ptr<SuccessorPolicy> successor;
        std::unique_ptr<LivenessPolicy> liveness;
        std::unique_ptr<RecoveryAuthorityPolicy> recoveryAuthority;
    } policy_;

    struct
    {
        // Cross-thread control: thread owned by ctor/dtor, stop/freeze are external
        // signals -- hence atomic.
        std::thread thread;
        std::atomic<bool> stop{false};
        std::atomic<bool> freeze{false};

        // Poll-thread-private below: single owner, so no atomics needed.
        std::uint32_t recoveryCycles{0};  // recovery takeover-span countdown
        PeerId recoveryTarget{NoPeer};    // peer the countdown belongs to; NoPeer when idle
        bool orphanSweepPending{false};   // free orphans next tick

        std::uint32_t periodicScanCountdown{0};  // cycles until next scan-scope refresh (0 => refresh first tick)
    } poll_;

    PeerTelemetry_t telemetry_;
};

}  // namespace cme
