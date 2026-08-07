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
#include "core/policy/successor_peterson_layout.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "core/types.hpp"
#include "observe/latency.hpp"
#include "util/coherency.hpp"
#include "util/time.hpp"

namespace cme
{

namespace
{

// Node, tree shape and climb path: successor_peterson_layout.hpp. Here, the protocol over them.
using peterson_tree::buildClimbPath;
using peterson_tree::ClimbStep_t;
using peterson_tree::getTournamentTreeBytes;
using peterson_tree::initTournamentTree;
using peterson_tree::PetersonNode_t;
using peterson_tree::roundUpPow2;

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

    // Reentrant only: the tournament is the lock and the record only shadows it, so record
    // residency must not admit anyone. > 1, not > 0, since the hold above already counts one.
    OBSERVE_LATENCY_BEGIN(Resident);
    auto& domain = peerState.getDomain(domainId);
    ownership_transfer::holdDomain(peerState, domainId);
    if (domain.getOwnershipPinCount() > 1)
    {
        OBSERVE_LATENCY_END(Resident, peerState, domainId);
        return OwnershipResult::Arrived;
    }

    // timeout==0 -> deadline==now, so acquire degenerates to a single probe (trylock);
    // on timeout it rolls back any partial climb, so no residency leaks.
    if (!state_->acquire(peerState, domainId, time::getMonoTime() + timeout))
    {
        ownership_transfer::unholdDomain(peerState, domainId);
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
