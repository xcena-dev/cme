// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// lifecycle.hpp -- peer membership lifecycle orchestration (join / leave / rejoin).
//
// Sibling to recovery.{hpp,cpp}; both orchestrate ownership_transfer primitives.

#pragma once

#include <cstdint>
#include <string_view>

#include "core/types.hpp"

namespace cme
{
class LocalPeerState;
}

namespace cme::lifecycle
{

enum class JoinResult
{
    Ok,
    CorruptRegion,  // own member slot or a per-domain SWPC line uninitialised
    UnknownDomain,  // entry not Active (deleted before we took control)
};

enum class LeaveResult
{
    Ok,
    NotParticipating,  // not joined -> nothing to leave (idempotent no-op)
    SoleParticipant,   // last active participant -> would orphan the domain
};

// Strategy-agnostic join: write own member slot, then per-domain sync DRAM
// holder state (claim vacant ownerships or note existing holder).
[[nodiscard]] JoinResult joinMembership(LocalPeerState& peerState);

// Strategy-agnostic leave: hand off held ownerships, mark slot NONE.
// noexcept: runs from ~Peer / ~LocalPeerState; must never throw out of a dtor.
void leaveMembership(LocalPeerState& peerState) noexcept;

// Baseline DRAM view then advertise participation. Idempotent.
// PRECONDITION: caller holds control lock (serialises against deleteDomain §1.8).
[[nodiscard]] JoinResult joinDomain(LocalPeerState& peerState, DomainId domainId);

// Refuse if sole participant (orphan risk; use deleteDomain §1.4); else hand off
// if held, retract participation.
// PRECONDITION: caller holds control lock (serialises against leave/delete).
[[nodiscard]] LeaveResult leaveDomain(LocalPeerState& peerState, DomainId domainId);

// Resolve a name to its Active data-domain id (live scan; skips control + Free).
// NoDomain and 0 if unknown.
//
// @outIncarnation comes from the visit that matched the name, and is not optional: asked as a second
// call, a delete and create between the two answer a pair naming a domain nobody resolved.
[[nodiscard]] DomainId findDataDomainByName(const LocalPeerState& peerState, std::string_view name,
                                            std::uint64_t& outIncarnation);

// The incarnation the slot carries now. A slot freed and claimed again reports a different one, which is
// what tells a handle kept across that from one still naming the domain it was resolved in.
// PRECONDITION: @domainId is in range; the record lookup is bare index arithmetic.
[[nodiscard]] std::uint64_t readDomainIncarnation(const LocalPeerState& peerState, DomainId domainId);

// ── dynamic domain registry (§1.3 / §1.4 / §1.6) ────────────────────

enum class CreateResult
{
    Ok,
    NoFreeSlot,     // every data domain entry is Active -- ceiling reached
    DuplicateName,  // an Active data domain already carries this name
    CorruptRegion,
};

// Claim a Free entry and publish it Active with @name; caller becomes genesis holder.
// PRECONDITION: caller holds the control lock. On Ok, *outDomainId = new domain.
[[nodiscard]] CreateResult createDomainLocked(LocalPeerState& peerState, std::string_view name,
                                              DomainId& outDomainId, std::uint64_t& outIncarnation);

enum class DeleteResult
{
    Ok,
    NotHolder,           // caller does not hold the target domain
    NotSoleParticipant,  // another peer still participates -- they must leave first
    CorruptRegion,
};

// Flip @domainId entry to Free. PRECONDITION: caller holds control + domain lock
// (dual-lock §1.4); re-checks participants ⊆ {self}.
[[nodiscard]] DeleteResult deleteDomainLocked(LocalPeerState& peerState, DomainId domainId);

// Free every orphan we hold (Active, recovery-taken, no participants).
// PRECONDITION: caller holds the control lock (no domain lock needed; no contender).
std::uint32_t reclaimOrphansLocked(LocalPeerState& peerState);

}  // namespace cme::lifecycle
