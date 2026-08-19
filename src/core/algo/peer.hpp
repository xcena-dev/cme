// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// peer.hpp -- one peer's SWOT instance + PeerGuard RAII.
//
// Owns LocalPeerState + Layer-0 oracles (Successor/Liveness/RecoveryAuthority).
// Ctor = create+join; dtor = leave+destroy. Strategy from region header.
//
//   cme::Peer peer{region, 1};
//   auto guard = peer.lock(0);  // throws on timeout; dtor unpins + transfers

#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string_view>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/types.hpp"
#include "observe/stats.hpp"

namespace cme
{

class Geometry;     // fwd
class PeerGuard;    // fwd
class OrderPolicy;  // fwd, SuccessorPolicy impl (friended by Peer)
class RequestPolicy;

struct LocalPeerState;

// TelemetrySnapshot_t + reader live in observe/stats.hpp.

class Peer
{
public:
    // ── rule of five ───────────────────────────────────────────────
    // Create + join. Throws JoinError on failure.
    // @coherency comes from the Session's format/open options; see cme::CoherencyMode.
    Peer(Geometry& geometry, PeerId peerId, CoherencyMode coherency);
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    Peer(Peer&&) noexcept;
    Peer& operator=(Peer&&) noexcept;
    ~Peer();  // leave + destroy

    // ── ops ────────────────────────────────────────────────────────
    // Throws LockTimeoutError if the global deadline elapses.
    [[nodiscard]] PeerGuard lock(DomainId domainId);

    // Non-throwing variant: nullopt on timeout.
    [[nodiscard]] std::optional<PeerGuard>
    tryLock(DomainId domainId,
            timing::Nanos timeout = timing::Secs{5});

    // ── participation (opt-in) ─────────────────────────────────────
    // lock() on non-joined domain throws NotParticipatingError.
    // leaveDomain throws it when sole participant. JoinError on range/corrupt.
    //
    // @expectedIncarnation refuses a slot that changed hands since the caller resolved the name it used.
    // Compared under the control lock, which is what create and delete take, so the answer is final.
    // nullopt for a caller naming a raw slot: an index has no incarnation to disagree with.
    void joinDomain(DomainId domainId, std::optional<std::uint64_t> expectedIncarnation = std::nullopt);
    void leaveDomain(DomainId domainId, std::optional<std::uint64_t> expectedIncarnation = std::nullopt);

    // ── dynamic domain registry ─────────────────────────────────────
    // Create a data domain; acquires control lock internally.
    // Throws DomainExistsError / DomainLimitError / LockTimeoutError.
    [[nodiscard]] DomainId createDomain(std::string_view name);
    // Delete @domainId (dual-lock: target + control). Throws if not sole participant / not holder.
    void deleteDomain(DomainId domainId, std::optional<std::uint64_t> expectedIncarnation = std::nullopt);

    // Which live slot carries @name, and the incarnation that same visit read. NoDomain and 0 if unknown.
    // One call for both, because two calls can straddle a delete and create and answer with the
    // incarnation of the domain that took the slot.
    [[nodiscard]] DomainId resolveDomainName(std::string_view name, std::uint64_t& outIncarnation) const;

    // The incarnation @domainId's slot carries now, or 0 for a slot holding no live domain. A caller
    // that kept an id compares this to tell its own domain from whatever claimed the slot after it.
    [[nodiscard]] std::uint64_t readDomainIncarnation(DomainId domainId) const;

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] PeerId getPeerId() const noexcept;
    // The regime this peer was opened with; callers outside LocalPeerState need it too.
    [[nodiscard]] CoherencyMode getCoherencyMode() const noexcept;

    // Cumulative counters snapshot.
    [[nodiscard]] TelemetrySnapshot_t getTelemetry() const;

    // Test-only: pause heartbeat bumping to simulate a dead peer and
    // exercise the SWOT recovery path end-to-end.
    void setFreeze(bool frozen) noexcept;

private:
    friend class PeerGuard;
    // SuccessorPolicy reaches into LocalPeerState via this back-channel.
    // LivenessPolicy only needs LocalPeerState's public surface.
    friend class OrderPolicy;
    friend class RequestPolicy;

    void releaseInternal(DomainId domainId) noexcept;  // used by PeerGuard dtor

    // Policy-oracle back-channel; reference stable for Peer lifetime.
    [[nodiscard]] LocalPeerState& getPeerState() noexcept;

private:
    std::unique_ptr<LocalPeerState> impl_;
};

// RAII lock holder: pins SWOT ownership (Use phase); dtor unpins to allow transfer.
class [[nodiscard]] PeerGuard
{
public:
    // ── rule of five ───────────────────────────────────────────────
    PeerGuard() noexcept = default;  // empty / null guard
    PeerGuard(const PeerGuard&) = delete;
    PeerGuard& operator=(const PeerGuard&) = delete;
    PeerGuard(PeerGuard&& other) noexcept;
    PeerGuard& operator=(PeerGuard&& other) noexcept;
    ~PeerGuard() noexcept;

    // ── ops ────────────────────────────────────────────────────────
    // Explicit early release. Idempotent.
    void release() noexcept;

    // ── accessors ──────────────────────────────────────────────────
    explicit operator bool() const noexcept
    {
        return peer_ != nullptr;
    }

private:
    friend class Peer;
    PeerGuard(Peer* peer, DomainId domainId) noexcept
        : peer_{peer},
          domainId_{domainId}
    {
    }

private:
    Peer* peer_ = nullptr;
    DomainId domainId_ = 0;
};

}  // namespace cme
