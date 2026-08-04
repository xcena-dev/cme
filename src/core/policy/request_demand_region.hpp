// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// request_demand_region.hpp -- RequestPolicy/RequestAgg private demand region.
//
// "Peer p wants domain d" is policy-private FAM state, so it lives in the successor area
// rather than Member_t. One whole cacheline per peer -- sub-line packing would let a 64B
// coherency::set clobber a neighbour's bits on noncoherent FAM. Self is the sole writer.
// Base-keyed accessors serve the Inspector, which has only a region base; the LocalPeerState
// helpers serve the policies. The DRAM self-shadow and write serialisation live there.

#pragma once

#include <cstdint>

#include "core/domain_bitmap.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "util/coherency.hpp"
#include "util/endian.hpp"

namespace cme::request_demand
{

// Per-peer demand line (one cacheline). pending[] mirrors the layout of the old
// Member_t::pendingDomains bitmap; the rest pads to 64B. Zeroed by Geometry::format's
// memset, so an unwritten / dead peer's line reads as "no demand" (no magic needed).
struct DemandLine_t
{
    endian::Field_t<std::uint64_t> pending[DomainWordCount];
    // Whatever pending[] leaves of the line. sizeof rather than 8, so the padding follows the
    // field type instead of having to be corrected after it changes. Asserted before the
    // subtraction: both operands are unsigned, so a bitmap that no longer fits would wrap into
    // a huge array rather than say which limit it broke.
    static constexpr std::uint32_t NamedBytes =
        sizeof(endian::Field_t<std::uint64_t>) * DomainWordCount;
    static_assert(NamedBytes < CacheLineBytes,
                  "DemandLine_t: pending[] leaves no pad; lower MaxDomains");

    std::uint8_t reserved[CacheLineBytes - NamedBytes];

    [[nodiscard]] DomainBitmap load() const noexcept
    {
        DomainBitmap bits;
        for (std::uint32_t word = 0; word < DomainWordCount; ++word)
        {
            bits.setWord(word, pending[word]);
        }
        return bits;
    }
    void store(DomainBitmap bits) noexcept
    {
        for (std::uint32_t word = 0; word < DomainWordCount; ++word)
        {
            pending[word] = bits.getWord(word);
        }
    }
};

static_assert(sizeof(DemandLine_t) == CacheLineBytes, "DemandLine_t must be one cacheline");

// Region bytes: one line per peer, at the head of the successor area.
[[nodiscard]] inline std::uint64_t getRegionBytes(std::uint32_t peerCount) noexcept
{
    return static_cast<std::uint64_t>(peerCount) * sizeof(DemandLine_t);
}

[[nodiscard]] inline DemandLine_t* getLine(std::uint8_t* base, PeerId peerId) noexcept
{
    return reinterpret_cast<DemandLine_t*>(base) + peerId;
}

// Cross-peer read: whole-line get (rmb) -> bitmap. Empty when the strategy has no
// demand region (base == nullptr).
[[nodiscard]] inline DomainBitmap loadPending(std::uint8_t* base, PeerId peerId,
                                              CoherencyMode mode) noexcept
{
    return base != nullptr ? coherency::get(getLine(base, peerId), mode).load() : DomainBitmap{};
}

// Publish a peer's whole demand line (single-writer: self on its own line, or the RA
// scrubbing a dead peer's line). No-op when the strategy has no demand region.
inline void storePending(std::uint8_t* base, PeerId peerId, DomainBitmap bits,
                         CoherencyMode mode) noexcept
{
    if (base == nullptr)
    {
        return;
    }
    DemandLine_t line{};
    line.store(bits);
    coherency::set(getLine(base, peerId), line, mode);
}

// ── LocalPeerState-coupled helpers ─────────────────────────────────────────
// Self writes go through updateSelfPending/clearSelfPending so the DRAM shadow (poll
// read) and the FAM demand-line publish stay serialised under the peer's local mutex.

// Raise this peer's demand for @domainId (the request signal).
inline void raise(LocalPeerState& peerState, DomainId domainId) noexcept
{
    peerState.updateSelfPending(domainId, true, [&peerState](const DomainBitmap& bits) noexcept
                                {
                                    storePending(peerState.getSuccessorAreaBase(), peerState.getPeerId(), bits,
                                                 peerState.getCoherencyMode());
                                });
}

// Drop this peer's demand for @domainId (acquired or timed out).
inline void drop(LocalPeerState& peerState, DomainId domainId) noexcept
{
    peerState.updateSelfPending(domainId, false, [&peerState](const DomainBitmap& bits) noexcept
                                {
                                    storePending(peerState.getSuccessorAreaBase(), peerState.getPeerId(), bits,
                                                 peerState.getCoherencyMode());
                                });
}

// Drop every demand bit (leave/teardown).
inline void clearSelf(LocalPeerState& peerState) noexcept
{
    peerState.clearSelfPending([&peerState](const DomainBitmap& bits) noexcept
                               {
                                   storePending(peerState.getSuccessorAreaBase(), peerState.getPeerId(), bits,
                                                peerState.getCoherencyMode());
                               });
}

// Cross-peer read on the grant scan: what @peerId is currently requesting.
[[nodiscard]] inline DomainBitmap loadPeer(LocalPeerState& peerState, PeerId peerId) noexcept
{
    return loadPending(peerState.getSuccessorAreaBase(), peerId, peerState.getCoherencyMode());
}

// RA recovery scrub: clear a dead peer's demand line (RA single-writer; the dead peer
// won't write it back).
inline void scrub(LocalPeerState& peerState, PeerId deadPeerId) noexcept
{
    storePending(peerState.getSuccessorAreaBase(), deadPeerId, DomainBitmap{},
                 peerState.getCoherencyMode());
}

}  // namespace cme::request_demand
