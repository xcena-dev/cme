// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_orphan_churn.cpp -- recovery x dynamic-domain churn soak.
//
// The scripted recovery tests never run recovery WHILE the domain registry mutates. Here
// every peer locks, deletes and recreates a private domain (sole participant, genesis
// holder) while a churn thread freezes and thaws peers, so a frozen peer's domain becomes
// an orphan the sweep must reclaim under sustained contention.
//
// A peer frozen long enough to be recovered is DEAD on thaw and its ops throw, so the
// worker re-spawns a fresh Peer, mirroring a real service's re-admit. The churn thread
// only raises a freeze flag; the worker applies it, so nothing races on the Peer pointer.
//
// Tolerant by design -- ops racing recovery legitimately throw. The teeth are the
// end-state invariants: every peer made progress, no unexpected fatal, and no slot leak
// (a fresh peer can still create slots - workers domains).
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.
// --strategy selects the successor policy (order|request|request-agg|peterson).

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "admission/claim.hpp"
#include "cme/errors.hpp"
#include "common/timing.hpp"
#include "core/algo/peer.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

// The shared slot plus the two flags this case alone needs. Its worker is its own: a re-admit here
// claims a fresh peer id rather than keeping one, which the shared loop does not do.
struct ChurnSlot_t : harness::PeerSlot_t
{
    std::atomic<bool> ready{false};   // the worker has spawned, so churn may start on it
    std::atomic<bool> paused{false};  // worker idles its op loop while quiescing
};

constexpr cme::PeerId Workers = 4;  // concurrent worker threads
// Each (re-)admit claims a fresh slot (claimPeerSlot, like a real service), so a
// dead peer's slot stays Recovering until its RA finalizes it -- spares cover the slots
// in flight (workers + auditor + a few recovering at once).
constexpr cme::PeerId MaxPeers = Workers * 3 + 2;
constexpr cme::DomainId DataSlots = 8;  // >> Workers, room to detect leaks
constexpr cme::DomainId Ceiling = DataSlots + 1;

// Each worker keeps one private data domain it solely participates in, churning
// create/lock/delete on it. The churn thread's freezeReq flag (applied here)
// crashes it; if it was recovered, its ops throw JoinError and it re-spawns.
void worker(ChurnSlot_t* peerSlot)
{
    // Re-admit claims a FRESH slot (claimPeerSlot skips Recovering slots), modelling
    // a real service: a crashed peer's identity dies with it: the new incarnation is a
    // new slot. ps->peerId is just this worker thread's stable label (RNG/naming/freeze).
    auto spawn = [&]() -> bool
    {
        try
        {
            const cme::PeerId claimedId =
                cme::admission::claimPeerSlot(*peerSlot->region, peerSlot->coherency);
            peerSlot->peer =
                std::make_unique<cme::Peer>(*peerSlot->region, claimedId, peerSlot->coherency);
            return true;
        }
        catch (const std::exception& e)
        {
            harness::log("worker %u spawn threw: %s", peerSlot->peerId, e.what());
            peerSlot->failed.store(true);
            return false;
        }
    };

    // (peer is worker-local: declared on the slot only so spawn() can set
    // it, but ONLY this thread ever reads/writes it.)
    if (!spawn())
    {
        peerSlot->ready.store(true);
        return;
    }
    peerSlot->ready.store(true);

    std::mt19937 rng{peerSlot->peerId + 1u};
    cme::DomainId privateId = cme::NoDomain;
    std::string privateName;
    std::uint32_t seq = 0;

    while (!peerSlot->stop.load(std::memory_order_acquire))
    {
        peerSlot->peer->setFreeze(peerSlot->frozen.load(std::memory_order_acquire));
        if (peerSlot->frozen.load() || peerSlot->paused.load())
        {
            harness::sleepMs(20);
            continue;
        }
        try
        {
            if (privateId != cme::NoDomain &&
                harness::resolvedSlot(*peerSlot->peer, privateName) != privateId)
            {
                privateId = cme::NoDomain;  // reclaimed while we were away
            }

            if (privateId == cme::NoDomain)
            {
                privateName = "p" + std::to_string(peerSlot->peerId) + "_" + std::to_string(seq++);
                privateId = peerSlot->peer->createDomain(privateName);  // creator auto-joins
            }
            else if (rng() % 10u < 7u)
            {
                auto guard = peerSlot->peer->tryLock(privateId, timing::Millis{2});
            }
            else
            {
                peerSlot->peer->deleteDomain(privateId);
                privateId = cme::NoDomain;
            }
            peerSlot->acquires.fetch_add(1, std::memory_order_relaxed);
            // Think-time: without it the soak degenerates into a control-lock contention
            // storm rather than the moderate churn recovery is meant to see.
            harness::sleepMs(2);
        }
        catch (const cme::JoinError&)
        {
            // Fenced (recovered) or not-joined: re-admit a fresh peer, like a
            // real service. Only this thread touches peer -> no race.
            privateId = cme::NoDomain;
            peerSlot->peer.reset();
            if (!spawn())
            {
                return;
            }
            peerSlot->acquires.fetch_add(1, std::memory_order_relaxed);
        }
        catch (const std::exception&)
        {
            // Expected churn race, usually a control-lock timeout. The domain may still
            // exist and be ours, so keep privateId -- forgetting it here would leak the
            // domain and spawn a second. The loop-top resync drops it if it truly vanished.
            peerSlot->acquires.fetch_add(1, std::memory_order_relaxed);
        }
    }
    peerSlot->peer.reset();
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    harness::startLogClock();

    auto region = harness::createRegion(Ceiling, MaxPeers);

    harness::log("orphan churn: %u workers, %u data slots (%s, backend=%s)", Workers, DataSlots,
                 ctx.strategySuffix(), ctx.backendName());

    std::array<ChurnSlot_t, Workers> peers{};
    for (cme::PeerId i = 0; i < Workers; ++i)
    {
        peers[i].peerId = i;
        peers[i].region = &region;
        peers[i].coherency = ctx.coherency();
        peers[i].runner = std::thread{worker, &peers[i]};
    }
    for (cme::PeerId i = 0; i < Workers; ++i)
    {
        while (!peers[i].ready.load())
        {
            harness::sleepMs(5);
        }
    }

    // ── churn: random freeze/thaw for 10 s, always leave >=2 workers active ──
    harness::sleepMs(500);
    harness::log("churn 10 s (freeze/thaw every ~600 ms)");
    std::array<bool, Workers> frozen{};
    cme::PeerId frozenCount = 0;
    std::mt19937 rng{0xC0FFEEu};
    std::uniform_int_distribution<cme::PeerId> pick{0, Workers - 1};
    const timing::Deadline end{timing::Secs{10}};
    while (!end.expired())
    {
        const auto target = pick(rng);
        if (frozen[target])
        {
            peers[target].frozen.store(false);
            frozen[target] = false;
            --frozenCount;
        }
        else if (frozenCount + 2 < Workers)
        {
            peers[target].frozen.store(true);
            frozen[target] = true;
            ++frozenCount;
        }
        harness::sleepMs(600);
    }

    // ── quiesce: thaw all, idle the op loops, let recovery reclaim every orphan ──
    harness::log("thaw all + settle (recovery drains orphans, fenced peers re-admit)");
    for (cme::PeerId i = 0; i < Workers; ++i)
    {
        peers[i].frozen.store(false);
        peers[i].paused.store(true);
    }
    harness::sleepMs(5000);

    // ── leak audit: a fresh peer must still create >= (slots - workers) domains. ──
    std::uint32_t auditCreated = 0;
    {
        cme::Peer audit{region, cme::admission::claimPeerSlot(region, ctx.coherency()), ctx.coherency()};
        for (cme::DomainId k = 0; k < DataSlots; ++k)
        {
            try
            {
                (void)audit.createDomain("audit_" + std::to_string(k));
                ++auditCreated;
            }
            catch (const cme::DomainLimitError&)
            {
                break;
            }
        }
    }
    harness::log("audit created %u domains (expect >= %u)", auditCreated, DataSlots - Workers);

    for (cme::PeerId i = 0; i < Workers; ++i)
    {
        peers[i].stop.store(true);
    }
    for (cme::PeerId i = 0; i < Workers; ++i)
    {
        if (peers[i].runner.joinable())
        {
            peers[i].runner.join();
        }
    }

    for (cme::PeerId i = 0; i < Workers; ++i)
    {
        ctx.check(peers[i].acquires.load() > 0, "worker made forward progress");
        ctx.check(!peers[i].failed.load(), "worker hit no unexpected fatal");
    }
    ctx.check(auditCreated >= DataSlots - Workers,
              "slots not leaked: fresh peer created >= (slots - workers) domains");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
