// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor_peterson_layout.hpp -- PetersonPolicy's FAM-resident tournament tree.
//
// Where a peer's interest lives: the node, the tree sizing, and the leaf->root path a peer id maps
// to. The climb protocol stays in the .cpp. The path is deterministic from the peer id, which is
// what lets recovery rebuild a dead peer's climb.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cme/shared.hpp"
#include "core/types.hpp"
#include "util/coherency.hpp"

namespace cme::peterson_tree
{

// ── tournament node layout (FAM-resident) ──────────────────────────
// Interest and fairness turn are distinct 64B line types, so get/set moves either as one
// single-writer transaction with no false sharing. FAM base is line-aligned, hence no alignas.

struct PetersonNode_t
{
    // A node is a 2-contender Peterson lock; Role names its two slots. A climber's
    // role at a node = which child it came up from (left -> Peer0, right -> Peer1).
    enum class Role : std::uint32_t
    {
        Peer0 = 0,
        Peer1 = 1,
    };

    // The opposing slot -- Peterson's "the other thread".
    [[nodiscard]] static constexpr Role other(Role role) noexcept
    {
        return role == Role::Peer0 ? Role::Peer1 : Role::Peer0;
    }

    // Peterson's flag[i], but identified: storing the peer id rather than a bare 0/1 lets
    // recovery clear only a dead peer's residue from a slot every climber shares over time.
    struct PeerLine_t
    {
        std::uint32_t owner = NoPeer;  // NoPeer = idle
        std::uint8_t reserved[CacheLineBytes - sizeof(std::uint32_t)]{};
    };
    // Tie-breaker line: turn = the role whose turn it is to yield.
    struct FairnessLine_t
    {
        std::uint32_t turn;
        std::uint8_t reserved[CacheLineBytes - sizeof(std::uint32_t)];
    };

    PeerLine_t peer[2];  // indexed by Role
    FairnessLine_t fairness;

    [[nodiscard]] PeerLine_t& getPeerLine(Role role) noexcept
    {
        return peer[static_cast<std::uint32_t>(role)];
    }
};

static_assert(sizeof(PetersonNode_t::PeerLine_t) == CacheLineBytes, "PeerLine_t must be one cacheline");
static_assert(sizeof(PetersonNode_t::FairnessLine_t) == CacheLineBytes, "FairnessLine_t must be one cacheline");
// Against the line types rather than the width: sizeof is already the size type, so the product needs
// no cast, and the two asserts above are what tie each line to one cacheline.
static_assert(sizeof(PetersonNode_t) ==
                  2 * sizeof(PetersonNode_t::PeerLine_t) + sizeof(PetersonNode_t::FairnessLine_t),
              "node spans 3 cachelines with no padding between them");
static_assert(offsetof(PetersonNode_t, fairness) == 2 * sizeof(PetersonNode_t::PeerLine_t),
              "fairness on its own line");

// ── helpers ─────────────────────────────────────────────────────────

// Smallest power of two >= n (min 1). The tournament needs a 2^k leaf count;
// peers above the live count map to spare leaves that never contend.
[[nodiscard]] inline std::uint32_t roundUpPow2(std::uint32_t count) noexcept
{
    std::uint32_t rounded = 1;
    while (rounded < count)
    {
        rounded <<= 1;
    }
    return rounded;
}

// Reverse the low @bits of @value -- the leaf permutation described in successor_peterson.cpp.
[[nodiscard]] inline std::uint32_t reverseBits(std::uint32_t value, std::uint32_t bits) noexcept
{
    std::uint32_t reversed = 0;
    for (std::uint32_t bit = 0; bit < bits; ++bit)
    {
        reversed = (reversed << 1) | (value & 1u);
        value >>= 1;
    }
    return reversed;
}

// ── tournament tree sizing / init ───────────────────────────────────
// The tree has roundUpPow2(maxNumPeers) leaves; `domains` independent trees are
// laid out contiguously: tree d owns nodes [d*(leaves-1), (d+1)*(leaves-1)).

[[nodiscard]] inline std::uint32_t getNodesPerDomain(std::uint32_t maxNumPeers) noexcept
{
    return roundUpPow2(maxNumPeers) - 1;
}

[[nodiscard]] inline std::uint64_t getTournamentTreeBytes(std::uint32_t maxNumPeers,
                                                          std::uint32_t domains) noexcept
{
    return static_cast<std::uint64_t>(sizeof(PetersonNode_t)) * getNodesPerDomain(maxNumPeers) *
           domains;
}

inline void initTournamentTree(std::uint8_t* base, std::uint32_t maxNumPeers,
                               std::uint32_t domains, CoherencyMode mode) noexcept
{
    auto* nodes = reinterpret_cast<PetersonNode_t*>(base);
    const std::uint32_t total = getNodesPerDomain(maxNumPeers) * domains;
    for (std::uint32_t index = 0; index < total; ++index)
    {
        coherency::set(&nodes[index].peer[0], PetersonNode_t::PeerLine_t{}, mode);
        coherency::set(&nodes[index].peer[1], PetersonNode_t::PeerLine_t{}, mode);
        coherency::set(&nodes[index].fairness, PetersonNode_t::FairnessLine_t{}, mode);
    }
}

// One level on a peer's climb: (0-based node index within a tree, contender slot).
struct ClimbStep_t
{
    std::uint32_t node;
    PetersonNode_t::Role role;
};

// A peer's fixed leaf -> root path, as (0-based node, role) per level; the tree shape is
// domain-independent. Deterministic from tid, so recovery can rebuild a dead peer's path.
[[nodiscard]] inline std::vector<ClimbStep_t> buildClimbPath(std::uint32_t maxNumPeers, PeerId tid)
{
    const std::uint32_t numLeaves = roundUpPow2(maxNumPeers);
    const auto depth = static_cast<std::uint32_t>(__builtin_ctz(numLeaves));
    std::uint32_t cur = numLeaves + reverseBits(tid, depth);
    std::vector<ClimbStep_t> path;
    while (cur > 1)
    {
        const std::uint32_t parent = cur / 2;  // heap index, 1-based
        // left child (even) -> Peer0, right child (odd) -> Peer1.
        const auto role = static_cast<PetersonNode_t::Role>(cur & 1u);
        path.push_back({parent - 1, role});  // store 0-based node index
        cur = parent;
    }
    return path;
}

// @domainId's own tree, which a ClimbStep_t's node index is relative to. Null when the strategy
// laid out no tree.
[[nodiscard]] inline PetersonNode_t* getTreeBase(std::uint8_t* areaBase, std::uint32_t maxNumPeers,
                                                 DomainId domainId) noexcept
{
    if (areaBase == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<PetersonNode_t*>(areaBase) +
           static_cast<std::size_t>(domainId) * getNodesPerDomain(maxNumPeers);
}

}  // namespace cme::peterson_tree
