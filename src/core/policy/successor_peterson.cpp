// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor_peterson.cpp -- tournament-Peterson SuccessorPolicy.
//
// The tournament IS the lock: winning the root == holding the domain, so the worker owns the
// whole lifecycle and pollCycle stays empty. One tree of (2^k - 1) Peterson nodes per domain,
// climbed with plain 64B load/store plus fences -- no atomics. leaf = numLeaves +
// bitReverse(peerId) is a fixed permutation, so membership cannot change under a held lock,
// occupancy stays balanced, and an absent peer's leaf auto-yields to its sibling.
//
// Ported from the tournament-Peterson baseline (J. Suetterlein et al., "Synchronization for
// CXL Based Memory", MEMSYS'24, Algorithm 1 + tree).

#include "core/policy/successor_peterson.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "cme/shared.hpp"
#include "config.hpp"
#include "core/algo/ownership_transfer.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/latency.hpp"
#include "util/coherency.hpp"
#include "util/time.hpp"

namespace cme
{

namespace
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
static_assert(sizeof(PetersonNode_t) == 3 * CacheLineBytes, "node spans 3 cachelines");
static_assert(offsetof(PetersonNode_t, fairness) == 2 * CacheLineBytes, "fairness on its own line");

// ── per-node 2-thread Peterson ──────────────────────────────────────

// One tournament level. Returns false, clearing its own interest, if @deadline passes
// before the level is won; a 0ns deadline degenerates to a single probe (trylock).
[[nodiscard]] bool lockNode(PetersonNode_t& node, PetersonNode_t::Role role,
                            time::TimePoint deadline,
                            LocalPeerState& peerState, DomainId domainId) noexcept
{
    using Role = PetersonNode_t::Role;
    const Role other = PetersonNode_t::other(role);

    OBSERVE_LATENCY_BEGIN(ClimbAnnounce);
    PetersonNode_t::PeerLine_t mine{};
    mine.owner = peerState.getPeerId();
    coherency::set(&node.getPeerLine(role), mine, peerState.getCoherencyMode());  // announce my interest + wmb

    PetersonNode_t::FairnessLine_t fairness{};
    fairness.turn = static_cast<std::uint32_t>(other);
    coherency::set(&node.fairness, fairness, peerState.getCoherencyMode());  // yield the turn + wmb

    coherency::mb();  // StoreLoad: turn-store before the other interest's load
    OBSERVE_LATENCY_END(ClimbAnnounce, peerState, domainId);

    OBSERVE_LATENCY_BEGIN(ClimbSpin);
    time::SpinBackoff backoff{SpinPausesMin, SpinPausesMax};
    bool won = false;
    while (true)
    {
        const std::uint32_t otherOwner = coherency::get(&node.getPeerLine(other), peerState.getCoherencyMode()).owner;  // rmb + read
        const Role turn = static_cast<Role>(coherency::get(&node.fairness, peerState.getCoherencyMode()).turn);
        // Proceed once the other slot is idle OR the turn is no longer theirs.
        if (otherOwner == NoPeer || turn != other)
        {
            won = true;
            break;
        }
        if (time::getMonoTime() >= deadline)
        {
            break;  // deadline; back out below
        }
        time::relaxCpu(backoff.next());
    }
    OBSERVE_LATENCY_END(ClimbSpin, peerState, domainId);

    if (!won)
    {
        coherency::set(&node.getPeerLine(role), PetersonNode_t::PeerLine_t{},
                       peerState.getCoherencyMode());  // retract announce
    }
    return won;
}

void unlockNode(PetersonNode_t& node, PetersonNode_t::Role role, CoherencyMode mode) noexcept
{
    coherency::set(&node.getPeerLine(role), PetersonNode_t::PeerLine_t{}, mode);  // clear my interest
}

// ── helpers ─────────────────────────────────────────────────────────

// Smallest power of two >= n (min 1). The tournament needs a 2^k leaf count;
// peers above the live count map to spare leaves that never contend.
[[nodiscard]] std::uint32_t roundUpPow2(std::uint32_t count) noexcept
{
    std::uint32_t rounded = 1;
    while (rounded < count)
    {
        rounded <<= 1;
    }
    return rounded;
}

// Reverse the low @bits of @value -- the leaf permutation described in the file header.
[[nodiscard]] std::uint32_t reverseBits(std::uint32_t value, std::uint32_t bits) noexcept
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

[[nodiscard]] std::uint64_t getTournamentTreeBytes(std::uint32_t maxNumPeers,
                                                   std::uint32_t domains) noexcept
{
    const std::uint32_t leaves = roundUpPow2(maxNumPeers);
    return static_cast<std::uint64_t>(sizeof(PetersonNode_t)) * (leaves - 1) * domains;
}

void initTournamentTree(std::uint8_t* base, std::uint32_t maxNumPeers, std::uint32_t domains,
                        CoherencyMode mode) noexcept
{
    auto* nodes = reinterpret_cast<PetersonNode_t*>(base);
    const std::uint32_t total = (roundUpPow2(maxNumPeers) - 1) * domains;
    for (std::uint32_t i = 0; i < total; ++i)
    {
        coherency::set(&nodes[i].peer[0], PetersonNode_t::PeerLine_t{}, mode);
        coherency::set(&nodes[i].peer[1], PetersonNode_t::PeerLine_t{}, mode);
        coherency::set(&nodes[i].fairness, PetersonNode_t::FairnessLine_t{}, mode);
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
[[nodiscard]] std::vector<ClimbStep_t> buildClimbPath(std::uint32_t maxNumPeers, PeerId tid)
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

}  // namespace

// ── per-peer climb state ────────────────────────────────────────────
// Non-owning view of the FAM node area plus this peer's precomputed leaf->root path. The
// tree shape is domain-independent, so one path serves every domain's tree base.

struct PetersonState_t
{
    PetersonState_t(std::uint8_t* areaBase, std::uint32_t maxNumPeers, PeerId tid,
                    CoherencyMode mode) noexcept
        : nodes_{reinterpret_cast<PetersonNode_t*>(areaBase)},
          nodesPerDomain_{roundUpPow2(maxNumPeers) - 1},
          maxNumPeers_{maxNumPeers},
          path_{buildClimbPath(maxNumPeers, tid)},
          mode_{mode}
    {
    }

    // Climb leaf -> root, bounded by @deadline. On a level timeout, release the
    // levels already won (root-relative reverse order) so no partial residency
    // leaks, and return false. true = root reached (holder).
    [[nodiscard]] bool acquire(LocalPeerState& peerState, DomainId domain,
                               time::TimePoint deadline) noexcept
    {
        const std::uint32_t base = domain * nodesPerDomain_;
        for (std::size_t won = 0; won < path_.size(); ++won)
        {
            const ClimbStep_t& step = path_[won];
            if (lockNode(nodes_[base + step.node], step.role, deadline, peerState, domain))
            {
                continue;
            }
            // Level @won timed out (already cleared its own interest); back out [0, won).
            for (std::size_t i = won; i-- > 0;)
            {
                unlockNode(nodes_[base + path_[i].node], path_[i].role, mode_);
            }
            return false;
        }
        return true;
    }

    void release(DomainId domain) noexcept
    {
        const std::uint32_t base = domain * nodesPerDomain_;
        for (auto step = path_.rbegin(); step != path_.rend(); ++step)  // root -> leaf
        {
            unlockNode(nodes_[base + step->node], step->role, mode_);
        }
    }

    // Clear @deadPeerId's stale interest, which would otherwise pin a sibling climber spinning
    // to its deadline. Only slots still holding deadPeerId, so a live announce survives.
    // root->leaf like release(): this scrub is what unpins the climber, so clearing leaf-first
    // would release it into levels not yet scrubbed and race its announce there.
    void cleanupDeadPeer(LocalPeerState& peerState, PeerId deadPeerId) noexcept
    {
        const std::vector<ClimbStep_t> deadPath = buildClimbPath(maxNumPeers_, deadPeerId);
        const std::uint32_t numDomains = peerState.getNumDomains();
        for (auto step = deadPath.rbegin(); step != deadPath.rend(); ++step)
        {
            for (std::uint32_t domainIdx = 0; domainIdx < numDomains; ++domainIdx)
            {
                PetersonNode_t& node = nodes_[domainIdx * nodesPerDomain_ + step->node];
                if (coherency::get(&node.getPeerLine(step->role), mode_).owner == deadPeerId)
                {
                    unlockNode(node, step->role, mode_);
                }
            }
        }
    }

private:
    PetersonNode_t* nodes_;
    std::uint32_t nodesPerDomain_;  // (numThreads-1); domain d's tree base = d*this
    std::uint32_t maxNumPeers_;     // for reconstructing any peer's climb path
    std::vector<ClimbStep_t> path_;
    CoherencyMode mode_;  // this peer's regime, captured at bind
};

// ── PetersonPolicy ──────────────────────────────────────────────────

PetersonPolicy::PetersonPolicy() noexcept = default;
PetersonPolicy::~PetersonPolicy() = default;

void PetersonPolicy::bind(LocalPeerState& peerState) noexcept
{
    state_ = std::make_unique<PetersonState_t>(peerState.getSuccessorAreaBase(),
                                               peerState.getMaxPeers(), peerState.getPeerId(),
                                               peerState.getCoherencyMode());
}

OwnershipResult PetersonPolicy::lock(LocalPeerState& peerState, DomainId domainId,
                                     std::chrono::nanoseconds timeout)
{
    // bind() builds state_ before the poll thread spawns; a null here means the policy was
    // never bound (format-path helper), so bail instead of faulting.
    if (!state_)
    {
        return OwnershipResult::NotArrived;
    }

    // Fast path: already holder (single-peer or cached residency) -> no tournament.
    OBSERVE_LATENCY_BEGIN(Resident);
    if (ownership_transfer::holdAndCheckResident(peerState, domainId))
    {
        OBSERVE_LATENCY_END(Resident, peerState, domainId);
        return OwnershipResult::Arrived;
    }

    // timeout==0 -> deadline==now, so acquire degenerates to a single probe (trylock);
    // on timeout it rolls back any partial climb, so no residency leaks.
    if (!state_->acquire(peerState, domainId, time::getMonoTime() + timeout))
    {
        return OwnershipResult::NotArrived;
    }

    // Tournament win = holder, and the prior holder vacated before release(), so the record
    // is the frontier. Passing it as the takeover base skips a shadow scan on the hot path.
    OBSERVE_LATENCY_BEGIN(Takeover);
    ownership_transfer::takeoverOwnership(peerState, domainId,
                                          peerState.loadDomainRecordSnapshot(domainId));
    OBSERVE_LATENCY_END(Takeover, peerState, domainId);
    return OwnershipResult::Arrived;
}

void PetersonPolicy::unlock(LocalPeerState& peerState, DomainId domainId)
{
    OBSERVE_LATENCY_BEGIN(Unlock);
    auto& domain = peerState.getDomain(domainId);

    // Vacate and release while the pin is still held, unpin LAST: the pin is what stops
    // pollCycle collecting this domain and racing the worker on the shared tournament.
    const bool outermost = domain.getOwnershipPinCount() == 1;

    // Vacate the record BEFORE release so a new winner's self-stamp isn't clobbered.
    // Unconditional here -- "unlock always vacates" is what lets pollCycle stay empty.
    if (outermost)
    {
        OBSERVE_LATENCY_BEGIN(Release);
        ownership_transfer::vacateOwnership(peerState, domainId);
        if (state_)
        {
            state_->release(domainId);
        }
        OBSERVE_LATENCY_END(Release, peerState, domainId);
    }

    domain.unpinOwnership();
    OBSERVE_LATENCY_END(Unlock, peerState, domainId);
}

void PetersonPolicy::pollCycle(LocalPeerState& /*peerState*/)
{
    // Empty: tournament-is-lock, so the worker owns the whole lifecycle (acquire in lock,
    // release in unlock) -- no token to forward. Steady-state transfer here raced the
    // worker re-acquiring the shared tournament -> double-hold. Dead peers: scrubRecoveredPeer.
}

void PetersonPolicy::scrubRecoveredPeer(LocalPeerState& peerState, PeerId deadPeerId) noexcept
{
    if (state_)
    {
        state_->cleanupDeadPeer(peerState, deadPeerId);
    }
}

std::uint64_t PetersonPolicy::getRegionAreaSize(std::uint32_t domainCount, std::uint32_t peerCount,
                                                std::uint32_t /*aggregatorGroups*/) const noexcept
{
    // [tournament trees only].
    return getTournamentTreeBytes(peerCount, domainCount);
}

void PetersonPolicy::format(std::uint8_t* successorAreaBase, std::uint32_t domainCount,
                            std::uint32_t peerCount, std::uint32_t /*aggregatorGroups*/,
                            CoherencyMode mode) const noexcept
{
    if (successorAreaBase == nullptr)
    {
        return;
    }
    initTournamentTree(successorAreaBase, peerCount, domainCount, mode);
}

}  // namespace cme
