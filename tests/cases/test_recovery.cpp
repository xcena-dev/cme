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
//
// What a crash phase asserts is read from the region: the dead peer's slot reaches None and no
// domain record still names it. A survivor's acquire counter is a per-domain figure here for the
// same reason -- a total across domains keeps rising on the ones the survivor always reached.

#include <algorithm>
#include <array>
#include <atomic>
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
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "cme/errors.hpp"
#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

constexpr cme::DomainId NumDomains = 4;
// Slot ceiling = control(0) + NumDomains data domains.
constexpr cme::DomainId DomainCeiling = NumDomains + 1;

// One deadline for a whole phase rather than one per check. Recovery is wall-clock-timed, so the
// post-conditions have to be polled; charging the deadline once per dead peer and once per domain
// would let a single broken phase spend the run's whole time budget before it named what failed.
constexpr std::uint32_t PhaseDeadlineMs = 15000;

enum class PState
{
    Running,
    Paused,
    Stop,  // graceful: dtor leaves membership
    Dead,
};

// The shared slot plus what this case drives its workers with. `state` adds a Paused step to the
// base's `stop`, and perDomain answers what the base's single counter cannot: which domain an
// acquire landed on.
struct TimelineSlot_t : harness::PeerSlot_t
{
    std::atomic<PState> state{PState::Running};
    std::array<std::atomic<std::uint64_t>, DomainCeiling> perDomain{};
};

void worker(TimelineSlot_t* slot)
{
    // Create our Peer and join the data domains. Only this thread touches ps->peer;
    // main drives freeze via ps->frozen, so no race. A frozen peer stays frozen
    // for good (permanent-crash model) -- recovery hands its domains to survivors.
    auto spawn = [&]() -> bool
    {
        try
        {
            slot->peer = std::make_unique<cme::Peer>(*slot->region, slot->peerId, slot->coherency);
            for (cme::DomainId domainId = 1; domainId <= slot->domainCount; ++domainId)
            {
                slot->peer->joinDomain(domainId);
            }
            return true;
        }
        catch (const std::exception& e)
        {
            harness::log("peer %u: ctor exception: %s", slot->peerId, e.what());
            slot->failed.store(true);
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
        slot->peer->setFreeze(slot->frozen.load(std::memory_order_acquire));
        if (status == PState::Paused || slot->frozen.load(std::memory_order_acquire))
        {
            harness::sleepMs(50);
            continue;
        }
        try
        {
            auto guard = slot->peer->lock(domainId);
            slot->acquires.fetch_add(1, std::memory_order_relaxed);
            slot->perDomain[domainId].fetch_add(1, std::memory_order_relaxed);
        }
        catch (const cme::LockTimeoutError&)
        {
            harness::log("peer %u: lock(d=%u) timed out (takeover path)", slot->peerId, domainId);
        }
        domainId = (domainId % slot->domainCount) + 1;
    }
    slot->peer.reset();  // Peer dtor calls leave + destroy
    slot->state.store(PState::Dead);
}

void spawnPeer(TimelineSlot_t& peerSlot, cme::PeerId peerId, cme::Geometry& region,
               cme::DomainId domainCount)
{
    peerSlot.peerId = peerId;
    peerSlot.region = &region;
    peerSlot.coherency = harness::currentRun().coherency();
    peerSlot.domainCount = domainCount;
    peerSlot.state.store(PState::Running);
    peerSlot.frozen.store(false);
    peerSlot.acquires.store(0);
    for (std::atomic<std::uint64_t>& counter : peerSlot.perDomain)
    {
        counter.store(0);
    }
    peerSlot.failed.store(false);
    peerSlot.runner = std::thread{worker, &peerSlot};
}

// The plural, as harness::spawnPeerWorkers is for the shared worker. This case keeps its own
// worker -- the timeline adds a Paused state and logs each timeout -- so it keeps its own spawn.
void spawnPeers(std::vector<TimelineSlot_t>& peers, cme::PeerId count, cme::Geometry& region,
                cme::DomainId domainCount)
{
    for (cme::PeerId peerId = 0; peerId < count; ++peerId)
    {
        spawnPeer(peers[peerId], peerId, region, domainCount);
    }
}

// Per-peer acquire counters at a phase boundary.
std::vector<std::uint64_t> snapshotAcq(const std::vector<TimelineSlot_t>& peers, cme::PeerId count)
{
    std::vector<std::uint64_t> snap(count);
    for (cme::PeerId i = 0; i < count; ++i)
    {
        snap[i] = peers[i].acquires.load();
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
    const std::vector<TimelineSlot_t>& peers;
    const std::vector<std::uint64_t>& before;
    const std::vector<bool>& skip;
    cme::PeerId count;
};

// Whether every peer outside @baseline.skip has acquired at least once more since
// @baseline.before.
[[nodiscard]] bool survivorsAdvanced(const SurvivorBaseline_t& baseline)
{
    for (cme::PeerId peerId = 0; peerId < baseline.count; ++peerId)
    {
        if (!baseline.skip[peerId] &&
            baseline.peers[peerId].acquires.load() <= baseline.before[peerId])
        {
            return false;
        }
    }
    return true;
}

// The same, reported one peer at a time.
void expectSurvivorsAdvanced(harness::TestContext& ctx, const SurvivorBaseline_t& baseline,
                             const char* phase)
{
    for (cme::PeerId peerId = 0; peerId < baseline.count; ++peerId)
    {
        if (baseline.skip[peerId])
        {
            continue;
        }
        const auto current = baseline.peers[peerId].acquires.load();
        ctx.checkf(current > baseline.before[peerId],
                   "survivor peer %u advanced past %s (%" PRIu64 " -> %" PRIu64 ")", peerId, phase,
                   baseline.before[peerId], current);
    }
}

// Simulated crash. Main only raises the request; the worker applies it to its own Peer.
void freezePeer(TimelineSlot_t& slot)
{
    slot.state.store(PState::Paused);
    slot.frozen.store(true);
}

// Whether any record still hands @peerId the domain. takeoverDomainsHeldByDeadPeer walks every
// domain and moves the ones a dead peer is named in, so one left behind is a takeover that ran short.
[[nodiscard]] bool anyDomainNames(const cme::Geometry& region, cme::PeerId peerId)
{
    for (cme::DomainId domainId = 0; domainId < region.getDomainCount(); ++domainId)
    {
        if (harness::readDomainRecord(region, domainId).isHeldBy(peerId))
        {
            return true;
        }
    }
    return false;
}

// Acquires on @domainId by every peer outside @skip.
[[nodiscard]] std::uint64_t domainTotal(const std::vector<TimelineSlot_t>& peers, cme::PeerId count,
                                        const std::vector<bool>& skip, cme::DomainId domainId)
{
    std::uint64_t total = 0;
    for (cme::PeerId peerId = 0; peerId < count; ++peerId)
    {
        if (!skip[peerId])
        {
            total += peers[peerId].perDomain[domainId].load();
        }
    }
    return total;
}

// Per-domain totals at a phase boundary, over the peers the phase leaves running. Taken once the
// phase's departures have all been applied, so `skip` is the same set here and at the check.
[[nodiscard]] std::array<std::uint64_t, DomainCeiling>
snapshotDomains(const std::vector<TimelineSlot_t>& peers, cme::PeerId count,
                const std::vector<bool>& skip)
{
    std::array<std::uint64_t, DomainCeiling> snap{};
    for (cme::DomainId domainId = 0; domainId < DomainCeiling; ++domainId)
    {
        snap[domainId] = domainTotal(peers, count, skip, domainId);
    }
    return snap;
}

// What one phase owes once the peers it stopped are gone: each of them reclaimed, every data
// domain acquired again by a survivor, and every survivor past its own mark.
struct PhaseCheck_t
{
    const cme::Geometry& region;
    const SurvivorBaseline_t& baseline;
    const std::array<std::uint64_t, DomainCeiling>& before;
    std::vector<cme::PeerId> departed;
    const char* phase;
};

[[nodiscard]] bool phaseSettled(const PhaseCheck_t& want)
{
    for (const cme::PeerId peerId : want.departed)
    {
        if (!harness::hasMemberStatus(want.region, peerId, Status::None) ||
            anyDomainNames(want.region, peerId))
        {
            return false;
        }
    }
    for (cme::DomainId domainId = 1; domainId <= NumDomains; ++domainId)
    {
        if (domainTotal(want.baseline.peers, want.baseline.count, want.baseline.skip, domainId) <=
            want.before[domainId])
        {
            return false;
        }
    }
    return survivorsAdvanced(want.baseline);
}

// Poll all of it on one deadline, then report each part on its own.
void expectPhaseSettled(harness::TestContext& ctx, const PhaseCheck_t& want)
{
    static_cast<void>(harness::waitUntil([&want]
                                         {
                                             return phaseSettled(want);
                                         },
                                         PhaseDeadlineMs));

    for (const cme::PeerId peerId : want.departed)
    {
        ctx.checkf(harness::hasMemberStatus(want.region, peerId, Status::None),
                   "peer %u's slot came back to None past %s", peerId, want.phase);
        ctx.checkf(!anyDomainNames(want.region, peerId),
                   "no domain record still names departed peer %u", peerId);
    }
    // The takeover assertion. A domain stranded on the departed holder times every survivor out
    // for good, and only that domain's own counter shows it: the others keep a total moving.
    for (cme::DomainId domainId = 1; domainId <= NumDomains; ++domainId)
    {
        const auto current =
            domainTotal(want.baseline.peers, want.baseline.count, want.baseline.skip, domainId);
        ctx.checkf(current > want.before[domainId],
                   "a survivor acquired domain %u past %s (%" PRIu64 " -> %" PRIu64 ")", domainId,
                   want.phase, want.before[domainId], current);
    }
    expectSurvivorsAdvanced(ctx, want.baseline, want.phase);
}

// Freeze a random live peer every 800 ms until @duration elapses, never dropping below two
// survivors. Permanent-crash model: a churn freeze sticks, so @frozen only grows.
void runChurn(std::vector<TimelineSlot_t>& peers, cme::PeerId count, std::vector<bool>& frozen,
              timing::Secs duration)
{
    std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<cme::PeerId> pick{0, count - 1};
    const timing::Deadline end{duration};
    while (!end.expired())
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

void shutdownPeers(std::vector<TimelineSlot_t>& peers, cme::PeerId count)
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
        if (peers[i].runner.joinable())
        {
            peers[i].runner.join();
        }
    }
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    harness::startLogClock();

    constexpr cme::PeerId MaxPeers = 8;
    constexpr cme::PeerId InitialPeers = 6;
    constexpr cme::PeerId FreezeTarget = 1;
    constexpr cme::PeerId LeaveTarget = 2;
    constexpr cme::PeerId MultiA = 3;
    constexpr cme::PeerId MultiB = 5;

    auto region = harness::createRegion(DomainCeiling, MaxPeers);

    // Create the NumDomains data domains (slots 1..NumDomains) before peers start.
    harness::seedDataDomains(region, NumDomains);

    std::vector<TimelineSlot_t> peers(MaxPeers + 1);
    harness::log("starting %u peers (%s, domains=%u, max_peers=%u, backend=%s)", InitialPeers,
                 ctx.strategySuffix(), NumDomains, MaxPeers, ctx.backendName());
    spawnPeers(peers, InitialPeers, region, NumDomains);

    // ── Phase A: steady-state ──────────────────────────────────────
    harness::sleepMs(1000);
    const auto acqSnapshot = snapshotAcq(peers, InitialPeers);
    harness::log("steady-state acq snapshot");
    for (cme::PeerId i = 0; i < InitialPeers; ++i)
    {
        ctx.checkf(acqSnapshot[i] > 0, "peer %u acquired at least once (%" PRIu64 ")", i, acqSnapshot[i]);
    }

    // ── Phase B: freeze peer ───────────────────────────────────────
    // FreezeTarget stays frozen for good (permanent-crash model). The slot going None and no record
    // naming it any more are recovery's writes; the per-domain counters are where a domain left
    // stranded on it would show.
    harness::log("freezing peer %u (simulated crash)", FreezeTarget);
    const auto freezeSkip = skipSet(InitialPeers, {FreezeTarget});
    freezePeer(peers[FreezeTarget]);
    const auto freezeDomains = snapshotDomains(peers, InitialPeers, freezeSkip);

    const SurvivorBaseline_t freezeBase{peers, acqSnapshot, freezeSkip, InitialPeers};
    expectPhaseSettled(ctx, {region, freezeBase, freezeDomains, {FreezeTarget}, "freeze"});

    // ── Phase D: graceful leave ────────────────────────────────────
    // The dtor's leave is what gives the slot back, and the region is where it lands. A worker
    // thread that joined out says only that the thread ran, whatever the leave wrote.
    harness::log("graceful leave: peer %u stop + destroy", LeaveTarget);
    const auto leavePre = snapshotAcq(peers, InitialPeers);
    const auto leaveSkip = skipSet(InitialPeers, {LeaveTarget, FreezeTarget});
    peers[LeaveTarget].state.store(PState::Stop);
    peers[LeaveTarget].runner.join();
    const auto leaveDomains = snapshotDomains(peers, InitialPeers, leaveSkip);

    bool leftParticipating = false;
    for (cme::DomainId domainId = 1; domainId <= NumDomains; ++domainId)
    {
        leftParticipating =
            leftParticipating || harness::participatesIn(region, LeaveTarget, domainId);
    }
    ctx.checkf(!leftParticipating, "peer %u's leave cleared every participation bit it held",
               LeaveTarget);

    const SurvivorBaseline_t leaveBase{peers, leavePre, leaveSkip, InitialPeers};
    expectPhaseSettled(ctx, {region, leaveBase, leaveDomains, {LeaveTarget}, "leave"});

    // ── Phase E: rejoin same slot ──────────────────────────────────
    harness::log("rejoin slot %u", LeaveTarget);
    spawnPeer(peers[LeaveTarget], LeaveTarget, region, NumDomains);
    harness::sleepMs(2000);
    ctx.checkf(peers[LeaveTarget].acquires.load() > 0, "rejoined peer %u acquired",
               LeaveTarget);

    // ── Phase F: concurrent multi-freeze ───────────────────────────
    // Two crashes at once, so the two recoveries overlap. Each dead slot is asked for separately:
    // one of the two finishing is not the same answer as both.
    harness::log("concurrent freeze: peers %u and %u", MultiA, MultiB);
    const auto multiPre = snapshotAcq(peers, InitialPeers);
    const auto multiSkip = skipSet(InitialPeers, {MultiA, MultiB, FreezeTarget});
    freezePeer(peers[MultiA]);
    freezePeer(peers[MultiB]);
    const auto multiDomains = snapshotDomains(peers, InitialPeers, multiSkip);

    const SurvivorBaseline_t multiBase{peers, multiPre, multiSkip, InitialPeers};
    expectPhaseSettled(ctx, {region, multiBase, multiDomains, {MultiA, MultiB}, "multi-freeze"});

    // ── Phase G: random churn for 10 s ─────────────────────────────
    // Seed frozen[] with the peers already frozen in B/F so the survivor cap counts them.
    harness::log("random churn for 10 s (events every 800 ms)");
    auto frozen = skipSet(InitialPeers, {FreezeTarget, MultiA, MultiB});
    const auto churnPre = snapshotAcq(peers, InitialPeers);
    runChurn(peers, InitialPeers, frozen, timing::Secs{10});
    const auto churnDomains = snapshotDomains(peers, InitialPeers, frozen);

    // Every peer churn took, not only the ones it took last: a sweep that recovers the newest crash
    // and forgets an earlier one leaves exactly one slot behind.
    std::vector<cme::PeerId> departed;
    for (cme::PeerId peerId = 0; peerId < InitialPeers; ++peerId)
    {
        if (frozen[peerId])
        {
            departed.push_back(peerId);
        }
    }
    const SurvivorBaseline_t churnBase{peers, churnPre, frozen, InitialPeers};
    expectPhaseSettled(ctx, {region, churnBase, churnDomains, std::move(departed), "churn"});

    // ── Phase H: shutdown ──────────────────────────────────────────
    harness::log("clean shutdown");
    shutdownPeers(peers, InitialPeers);

    harness::log("final acq counts:");
    for (cme::PeerId i = 0; i < InitialPeers; ++i)
    {
        harness::log("  peer %u : %" PRIu64 "  rc=%d", i, peers[i].acquires.load(), peers[i].failed.load());
    }
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
