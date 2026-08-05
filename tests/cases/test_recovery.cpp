// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_recovery.cpp -- dynamic join / freeze / leave / rejoin / churn.
//
// Permanent-crash model: a frozen peer is dead for good, and survivors take its domains
// over. Scripted timeline:
//
//   t = 0.0s   start K active peers
//   t = 1.0s   steady-state acq check
//   t = 2.0s   freeze peer 1 (permanent crash); verify survivor takeover
//   graceful leave peer 2 -> rejoin slot 2
//   concurrent multi-freeze (peers 3, 5; permanent) -> verify survivor takeover
//   10 s random churn (permanent freezes, survivor cap >=2)
//   clean shutdown + final acq counts

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

enum class PState
{
    Running,
    Paused,
    Stop,  // graceful: dtor leaves membership
    Dead,
};

struct PeerSlot_t
{
    std::thread tid;
    std::unique_ptr<cme::Peer> peer;  // WORKER-THREAD-ONLY (main drives freeze via freezeReq)
    cme::Geometry* region{nullptr};
    cme::PeerId peerId{0};
    cme::CoherencyMode coherency{};
    cme::DomainId numDomains{0};
    std::atomic<PState> state{PState::Running};
    std::atomic<bool> freezeReq{false};  // main raises; worker applies to its own Peer
    std::atomic<std::uint64_t> acqCount{0};
    std::atomic<int> rc{0};
};

void worker(PeerSlot_t* slot)
{
    // Create our Peer and join the data domains. Only this thread touches ps->peer;
    // main drives freeze via ps->freezeReq, so no race. A frozen peer stays frozen
    // for good (permanent-crash model) -- recovery hands its domains to survivors.
    auto spawn = [&]() -> bool
    {
        try
        {
            slot->peer = std::make_unique<cme::Peer>(*slot->region, slot->peerId, slot->coherency);
            for (cme::DomainId domainId = 1; domainId <= slot->numDomains; ++domainId)
            {
                slot->peer->joinDomain(domainId);
            }
            return true;
        }
        catch (const std::exception& e)
        {
            harness::log("peer %u: ctor exception: %s", slot->peerId, e.what());
            slot->rc.store(1);
            return false;
        }
    };

    if (!spawn())
    {
        slot->state.store(PState::Dead);
        return;
    }

    cme::DomainId domainId = 1;
    while (true)
    {
        const auto status = slot->state.load(std::memory_order_acquire);
        if (status == PState::Stop)
        {
            break;
        }
        slot->peer->setFreeze(slot->freezeReq.load(std::memory_order_acquire));
        if (status == PState::Paused || slot->freezeReq.load(std::memory_order_acquire))
        {
            harness::sleepMs(50);
            continue;
        }
        try
        {
            auto guard = slot->peer->lock(domainId);
            slot->acqCount.fetch_add(1, std::memory_order_relaxed);
        }
        catch (const cme::LockTimeoutError&)
        {
            harness::log("peer %u: lock(d=%u) timed out (takeover path)", slot->peerId, domainId);
        }
        domainId = (domainId % slot->numDomains) + 1;
    }
    slot->peer.reset();  // Peer dtor calls leave + destroy
    slot->state.store(PState::Dead);
}

void spawnPeer(PeerSlot_t& peerSlot, cme::PeerId peerId, cme::Geometry* region,
               cme::DomainId numDomains, cme::CoherencyMode coherency)
{
    peerSlot.peerId = peerId;
    peerSlot.region = region;
    peerSlot.coherency = coherency;
    peerSlot.numDomains = numDomains;
    peerSlot.state.store(PState::Running);
    peerSlot.freezeReq.store(false);
    peerSlot.acqCount.store(0);
    peerSlot.rc.store(0);
    peerSlot.tid = std::thread{worker, &peerSlot};
}

// Per-peer acquire counters at a phase boundary.
std::vector<std::uint64_t> snapshotAcq(const std::vector<PeerSlot_t>& peers, cme::PeerId count)
{
    std::vector<std::uint64_t> snap(count);
    for (cme::PeerId i = 0; i < count; ++i)
    {
        snap[i] = peers[i].acqCount.load();
    }
    return snap;
}

std::vector<bool> skipSet(cme::PeerId count, std::initializer_list<cme::PeerId> ids)
{
    std::vector<bool> skip(count, false);
    for (const cme::PeerId peerId : ids)
    {
        if (peerId < count)
        {
            skip[peerId] = true;
        }
    }
    return skip;
}

// What a survivor check compares against: where each peer's counter stood before the phase,
// and which peers are excluded because this phase is the one that stopped them.
struct SurvivorBaseline_t
{
    const std::vector<PeerSlot_t>& peers;
    const std::vector<std::uint64_t>& before;
    const std::vector<bool>& skip;
    cme::PeerId count;
};

// Every peer outside @baseline.skip must have acquired at least once more since @baseline.before.
void expectSurvivorsAdvanced(harness::TestContext& ctx, const SurvivorBaseline_t& baseline,
                             const char* phase)
{
    for (cme::PeerId peerId = 0; peerId < baseline.count; ++peerId)
    {
        if (baseline.skip[peerId])
        {
            continue;
        }
        const auto current = baseline.peers[peerId].acqCount.load();
        ctx.checkf(current > baseline.before[peerId],
                   "survivor peer %u advanced past %s (%" PRIu64 " -> %" PRIu64 ")", peerId, phase,
                   baseline.before[peerId], current);
    }
}

// Simulated crash. Main only raises the request; the worker applies it to its own Peer.
void freezePeer(PeerSlot_t& slot)
{
    slot.state.store(PState::Paused);
    slot.freezeReq.store(true);
}

// A frozen peer's counter may still tick a few times before its worker notices.
void expectFlat(harness::TestContext& ctx, const PeerSlot_t& slot, std::uint64_t before,
                cme::PeerId peerId)
{
    constexpr std::uint64_t Slop = 4;
    const auto cur = slot.acqCount.load();
    ctx.checkf(cur - before <= Slop,
               "frozen peer %u flat (%" PRIu64 " -> %" PRIu64 ", slop<=%" PRIu64 ")", peerId, before,
               cur, Slop);
}

// Freeze a random live peer every 800 ms until @duration elapses, never dropping below two
// survivors. Permanent-crash model: a churn freeze sticks, so @frozen only grows.
void runChurn(std::vector<PeerSlot_t>& peers, cme::PeerId count, std::vector<bool>& frozen,
              std::chrono::seconds duration)
{
    std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<cme::PeerId> pick{0, count - 1};
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end)
    {
        const cme::PeerId target = pick(rng);
        const bool gone = peers[target].state.load() == PState::Dead || frozen[target];
        const auto frozenCount =
            static_cast<cme::PeerId>(std::count(frozen.begin(), frozen.end(), true));
        if (!gone && frozenCount + 2 <= count)
        {
            freezePeer(peers[target]);
            frozen[target] = true;
            harness::log("churn: freeze peer %u", target);
        }
        harness::sleepMs(800);
    }
}

void shutdownPeers(std::vector<PeerSlot_t>& peers, cme::PeerId count)
{
    for (cme::PeerId i = 0; i < count; ++i)
    {
        if (peers[i].state.load() != PState::Dead)
        {
            peers[i].state.store(PState::Stop);
        }
    }
    for (cme::PeerId i = 0; i < count; ++i)
    {
        if (peers[i].tid.joinable())
        {
            peers[i].tid.join();
        }
    }
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    harness::startLogClock();

    const cme::Strategy strategy = ctx.strategy();
    const char* const stratSuffix = ctx.strategySuffix();
    constexpr cme::DomainId NumDomains = 4;
    constexpr cme::PeerId MaxPeers = 8;
    constexpr cme::PeerId InitialPeers = 6;
    constexpr cme::PeerId FreezeTarget = 1;
    constexpr cme::PeerId LeaveTarget = 2;
    constexpr cme::PeerId MultiA = 3;
    constexpr cme::PeerId MultiB = 5;

    // Slot ceiling = control(0) + NumDomains data domains.
    constexpr cme::DomainId DomainCeiling = NumDomains + 1;
    std::optional<cme::Geometry> region;
    const cme::Geometry::FormatOpts_t fmtOpts{strategy};
    region.emplace(ctx.memory().createRegion(DomainCeiling, MaxPeers, fmtOpts));

    // Create the NumDomains data domains (slots 1..NumDomains) before peers start.
    harness::seedDataDomains(*region, NumDomains, ctx.coherency());

    std::vector<PeerSlot_t> peers(MaxPeers + 1);
    harness::log("starting %u peers (%s, domains=%u, max_peers=%u, backend=%s)", InitialPeers,
                 stratSuffix, NumDomains, MaxPeers, ctx.backendName());
    for (cme::PeerId i = 0; i < InitialPeers; ++i)
    {
        spawnPeer(peers[i], i, &*region, NumDomains, ctx.coherency());
    }

    // ── Phase A: steady-state ──────────────────────────────────────
    harness::sleepMs(1000);
    const auto acqSnapshot = snapshotAcq(peers, InitialPeers);
    harness::log("steady-state acq snapshot");
    for (cme::PeerId i = 0; i < InitialPeers; ++i)
    {
        ctx.checkf(acqSnapshot[i] > 0, "peer %u acquired at least once (%" PRIu64 ")", i, acqSnapshot[i]);
    }

    // ── Phase B: freeze peer ───────────────────────────────────────
    // FreezeTarget stays frozen for good (permanent-crash model); the survivor-progress
    // check is what confirms recovery reclaimed its domains.
    harness::log("freezing peer %u (simulated crash)", FreezeTarget);
    const auto bFreeze = peers[FreezeTarget].acqCount.load();
    freezePeer(peers[FreezeTarget]);

    harness::sleepMs(7500);  // > AcquireTimeout (config.hpp) so survivors take over
    expectFlat(ctx, peers[FreezeTarget], bFreeze, FreezeTarget);
    expectSurvivorsAdvanced(
        ctx, {peers, acqSnapshot, skipSet(InitialPeers, {FreezeTarget}), InitialPeers}, "freeze");

    // ── Phase D: graceful leave ────────────────────────────────────
    harness::log("graceful leave: peer %u stop + destroy", LeaveTarget);
    const auto dPre = snapshotAcq(peers, InitialPeers);
    peers[LeaveTarget].state.store(PState::Stop);
    peers[LeaveTarget].tid.join();
    ctx.checkf(peers[LeaveTarget].state.load() == PState::Dead,
               "peer %u joined out as Dead", LeaveTarget);

    harness::sleepMs(1500);
    expectSurvivorsAdvanced(
        ctx, {peers, dPre, skipSet(InitialPeers, {LeaveTarget, FreezeTarget}), InitialPeers},
        "leave");

    // ── Phase E: rejoin same slot ──────────────────────────────────
    harness::log("rejoin slot %u", LeaveTarget);
    spawnPeer(peers[LeaveTarget], LeaveTarget, &*region, NumDomains, ctx.coherency());
    harness::sleepMs(2000);
    ctx.checkf(peers[LeaveTarget].acqCount.load() > 0, "rejoined peer %u acquired",
               LeaveTarget);

    // ── Phase F: concurrent multi-freeze ───────────────────────────
    harness::log("concurrent freeze: peers %u and %u", MultiA, MultiB);
    const auto fPre = snapshotAcq(peers, InitialPeers);
    freezePeer(peers[MultiA]);
    freezePeer(peers[MultiB]);
    harness::sleepMs(7500);
    expectFlat(ctx, peers[MultiA], fPre[MultiA], MultiA);
    expectFlat(ctx, peers[MultiB], fPre[MultiB], MultiB);
    expectSurvivorsAdvanced(
        ctx, {peers, fPre, skipSet(InitialPeers, {MultiA, MultiB, FreezeTarget}), InitialPeers},
        "multi-freeze");

    // ── Phase G: random churn for 10 s ─────────────────────────────
    // Seed frozen[] with the peers already frozen in B/F so the survivor cap counts them.
    harness::log("random churn for 10 s (events every 800 ms)");
    auto frozen = skipSet(InitialPeers, {FreezeTarget, MultiA, MultiB});
    const auto gPre = snapshotAcq(peers, InitialPeers);
    runChurn(peers, InitialPeers, frozen, std::chrono::seconds{10});
    harness::sleepMs(4000);
    expectSurvivorsAdvanced(ctx, {peers, gPre, frozen, InitialPeers}, "churn");

    // ── Phase H: shutdown ──────────────────────────────────────────
    harness::log("clean shutdown");
    shutdownPeers(peers, InitialPeers);

    harness::log("final acq counts:");
    for (cme::PeerId i = 0; i < InitialPeers; ++i)
    {
        harness::log("  peer %u : %" PRIu64 "  rc=%d", i, peers[i].acqCount.load(), peers[i].rc.load());
    }
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
