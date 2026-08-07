// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_peterson_scrub.cpp -- recovery clears a dead climber's tournament interest (R3).
//
// Peterson's policy-private state is the interest announced at each node on a climb. A stale one
// satisfies neither exit of the sibling's spin, so it starves a live peer outright.
// One data domain, so the pinned sibling has nowhere else to go. Residue written directly, as in
// demand_scrub. peterson only: the other three lay out no tree, so the write would land on
// something else.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/layout/geometry.hpp"
#include "core/policy/successor_peterson_layout.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;
using Node = cme::peterson_tree::PetersonNode_t;

constexpr cme::PeerId MaxPeers = 4;
constexpr cme::DomainId NumDomains = 1;
constexpr cme::DomainId Ceiling = NumDomains + 1;  // control + data
constexpr cme::PeerId Dead = 1;                    // the climber that dies mid-announce
constexpr cme::DomainId Contested = 1;             // the only data domain, so the only tree in play

// Recovery is wall-clock-timed, so poll the post-condition rather than sleep once and read.
constexpr std::uint32_t RecoveryDeadlineMs = 20000;

// Several AcquireTimeouts' worth, rather than a tuned window.
constexpr std::uint32_t ProgressDeadlineMs = 5000;

// The peer the residue pins: @deadPeerId's opponent at the first node above their leaf pair.
// Derived, because the leaf permutation decides it.
[[nodiscard]] cme::PeerId findPinnedSibling(cme::PeerId deadPeerId)
{
    const auto deadPath = cme::peterson_tree::buildClimbPath(MaxPeers, deadPeerId);
    for (cme::PeerId peerId = 0; peerId < MaxPeers; ++peerId)
    {
        if (peerId == deadPeerId)
        {
            continue;
        }
        const auto path = cme::peterson_tree::buildClimbPath(MaxPeers, peerId);
        if (path[0].node == deadPath[0].node && path[0].role != deadPath[0].role)
        {
            return peerId;
        }
    }
    return cme::NoPeer;
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const auto coherency = ctx.coherency();

    auto region = harness::createRegion(Ceiling, MaxPeers);
    harness::seedDataDomains(region, NumDomains);

    std::printf("peterson scrub: %u peers (%s, backend=%s)\n",
                MaxPeers, ctx.strategySuffix(), ctx.backendName());

    std::array<harness::PeerSlot_t, MaxPeers> peers{};
    harness::spawnPeerWorkers(peers, MaxPeers, region, NumDomains);
    harness::sleepMs(1000);  // memberships go Active; ownership spreads
    ctx.check(harness::allPeersJoined(peers, MaxPeers), "every worker joined its domain");

    if (!ctx.check(region.getSuccessorAreaBase() != nullptr,
                   "the strategy has a tournament tree to scrub"))
    {
        harness::joinPeerWorkers(peers, MaxPeers);
        return;
    }

    const auto deadPath = cme::peterson_tree::buildClimbPath(MaxPeers, Dead);
    Node* tree = cme::peterson_tree::getTreeBase(region.getSuccessorAreaBase(), MaxPeers, Contested);
    Node::PeerLine_t* announce = &tree[deadPath[0].node].getPeerLine(deadPath[0].role);
    const auto ownerOf = [announce, coherency]
    {
        return cme::coherency::get(announce, coherency).owner;
    };

    const cme::PeerId sibling = findPinnedSibling(Dead);
    if (!ctx.check(sibling != cme::NoPeer, "the dead peer has a sibling at its first node"))
    {
        harness::joinPeerWorkers(peers, MaxPeers);
        return;
    }
    std::printf("peer %u announces at node %u; peer %u contends there\n",
                Dead, deadPath[0].node, sibling);

    // Inside the liveness grace window, so no survivor has started recovery yet. The write races
    // nobody: a node role is fixed by which child a climber came up from, so this slot is the dead
    // peer's alone.
    peers[Dead].frozen.store(true);
    harness::sleepMs(150);  // the worker applies setFreeze and stops climbing
    Node::PeerLine_t stale{};
    stale.owner = Dead;
    cme::coherency::set(announce, stale, coherency);

    ctx.check(ownerOf() == Dead, "the dead peer's announce stands at its first node");
    ctx.check(harness::hasMemberStatus(region, Dead, Status::Active),
              "its slot is still Active, so recovery has not run yet");

    const bool scrubbed = harness::waitUntil(
        [&ownerOf]
        {
            return ownerOf() == cme::NoPeer;
        },
        RecoveryDeadlineMs);
    ctx.check(scrubbed, "recovery cleared the dead peer's announce");
    ctx.check(harness::hasMemberStatus(region, Dead, Status::None),
              "and finished the slot it belonged to");

    // The sibling's counter is what proves the scrub released it rather than zeroing a line nobody
    // waited on. The others are checked with it, so a scrub that broke the tree elsewhere shows up.
    std::array<std::uint64_t, MaxPeers> before{};
    for (cme::PeerId index = 0; index < MaxPeers; ++index)
    {
        before[index] = peers[index].acquires.load();
    }
    const bool advanced = harness::waitUntil(
        [&peers, &before]
        {
            for (cme::PeerId index = 0; index < MaxPeers; ++index)
            {
                if (index != Dead && peers[index].acquires.load() <= before[index])
                {
                    return false;
                }
            }
            return true;
        },
        ProgressDeadlineMs);
    ctx.check(advanced, "every survivor advanced after recovery, the pinned sibling included");

    peers[Dead].abandon.store(true);  // no destructor over a slot recovery has freed
    harness::joinPeerWorkers(peers, MaxPeers);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
