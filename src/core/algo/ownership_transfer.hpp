// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// ownership_transfer.hpp -- strategy-agnostic SWOT mechanism primitives.
//
// Layer-1 primitives composing into the Layer-0 interface, touching the SWPC line tables in
// shared memory. Ownership is a single-writer "(epoch, owner)" cell whose advancing epoch is
// the SWOT monotonic counter (EpochImpliesIdentity).

#pragma once

#include <chrono>

#include "common/timing.hpp"
#include "core/domain_bitmap.hpp"
#include "core/layout/geometry.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"

namespace cme::ownership_transfer
{

// ── Peer-scoped helpers (defined in ownership_transfer.cpp) ───────

// Both advance the single-writer (holder, epoch) record; epoch bump demotes stale holders (P2).

// SWOT Transfer(to): advance the record to @newHolder, drop local holder belief.
// No doorbell ring -- the new holder discovers the handoff by polling the record.
void transferOwnership(LocalPeerState& peerState, DomainId domainId, PeerId newHolder);

// Same, under the per-domain transfer latch: re-checks pin==0 and blocks new pins across the
// publish, closing the TOCTOU between a grant decision and its record write. False = a local
// worker re-pinned, so it owns the domain. Use for every grant of a HELD domain.
[[nodiscard]] bool transferOwnershipGuarded(LocalPeerState& peerState, DomainId domainId,
                                            PeerId newHolder);

// Publish a record incarnation: holder-group shadow THEN truth, from this one site, so the
// truth=self => own-shadow=self invariant holds by construction. A waiter polls its own shadow
// and reads the truth only when that names it, so the shadow has to land first.
void publishDomainRecord(LocalPeerState& peerState, DomainId domainId,
                         const Geometry::DomainRecord_t& record);

// SWOT TakeoverOwnership: stamp self one epoch above @base. The caller supplies the copy to
// bump, and is the one that knows how far above the truth it has to reach.
void takeoverOwnership(LocalPeerState& peerState, DomainId domainId, Geometry::DomainRecord_t base);

// Model-A: stamp record NoPeer (release happens immediately after this by the caller).
// Must run BEFORE the tournament release so a new winner can't race our NoPeer write.
void vacateOwnership(LocalPeerState& peerState, DomainId domainId);

// Poll-thread liveness tick: stamps Geometry::Member_t::lastSeenNanos with the
// current wall clock, then mirrors poll/worker CPU times into the profile slot.
void bumpHeartbeat(LocalPeerState& peerState);

// Block until this peer's record line names us at a newer epoch (EpochImpliesIdentity), or
// @timeout elapses. timeout=0 is a single non-blocking probe -- no spin, no sleep.
[[nodiscard]] OwnershipResult waitForOwnership(LocalPeerState& peerState, DomainId domainId,
                                               const timing::Deadline& deadline);

// Reconcile holder belief across active domains and return the held+transferable set; only
// the check turn adopts vs the record. Sole writer of holder belief in the poll cycle.
DomainBitmap reconcileAndCollectTransferable(LocalPeerState& peerState);

// Local-belief-only collect, no per-domain FAM read. For REQUEST-family poll cycles, whose
// grants are handled by the requesting worker. Safety argument at the definition.
DomainBitmap collectTransferableLocal(LocalPeerState& peerState);

// Pin/unpin ownership around the SWOT Use phase.
// canTransferDomain: holder + zero pins = safe to transfer (Use-phase barrier).
void holdDomain(LocalPeerState& peerState, DomainId domainId);
// Fast path: pin, then report whether we already hold it -- lock Arrives with no acquire
// work. Shared by the successor-elect policies; ORDER waits unconditionally.
[[nodiscard]] bool holdAndCheckResident(LocalPeerState& peerState, DomainId domainId);
void unholdDomain(LocalPeerState& peerState, DomainId domainId);
[[nodiscard]] bool canTransferDomain(LocalPeerState& peerState, DomainId domainId) noexcept;

}  // namespace cme::ownership_transfer
