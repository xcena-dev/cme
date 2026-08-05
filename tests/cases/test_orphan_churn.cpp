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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>

#include "admission/claim.hpp"
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

struct PeerSlot_t
{
    std::thread tid;
    std::unique_ptr<cme::Peer> peerInstance;  // WORKER-THREAD-ONLY access
    cme::Geometry* region{nullptr};
    cme::PeerId peerId{0};
    cme::CoherencyMode coherency{};
    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> freezeReq{false};  // churn raises; worker applies to its Peer
    std::atomic<bool> paused{false};     // worker idles its op loop while quiescing
    std::atomic<std::uint64_t> ops{0};
    std::atomic<int> rc{0};
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
void worker(PeerSlot_t* peerSlot)
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
            peerSlot->peerInstance =
                std::make_unique<cme::Peer>(*peerSlot->region, claimedId, peerSlot->coherency);
            return true;
        }
        catch (const std::exception& e)
        {
            harness::log("worker %u spawn threw: %s", peerSlot->peerId, e.what());
            peerSlot->rc.store(1);
            return false;
        }
    };

    // (peerInstance is worker-local: declared on the slot only so spawn() can set
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
        peerSlot->peerInstance->setFreeze(peerSlot->freezeReq.load(std::memory_order_acquire));
        if (peerSlot->freezeReq.load() || peerSlot->paused.load())
        {
            harness::sleepMs(20);
            continue;
        }
        try
        {
            if (privateId != cme::NoDomain &&
                peerSlot->peerInstance->resolveDomainName(privateName) != privateId)
            {
                privateId = cme::NoDomain;  // reclaimed while we were away
            }

            if (privateId == cme::NoDomain)
            {
                privateName = "p" + std::to_string(peerSlot->peerId) + "_" + std::to_string(seq++);
                privateId = peerSlot->peerInstance->createDomain(privateName);  // creator auto-joins
            }
            else if (rng() % 10u < 7u)
            {
                auto guard = peerSlot->peerInstance->tryLock(privateId, std::chrono::milliseconds{2});
            }
            else
            {
                peerSlot->peerInstance->deleteDomain(privateId);
                privateId = cme::NoDomain;
            }
            peerSlot->ops.fetch_add(1, std::memory_order_relaxed);
            // Think-time: without it the soak degenerates into a control-lock contention
            // storm rather than the moderate churn recovery is meant to see.
            harness::sleepMs(2);
        }
        catch (const cme::JoinError&)
        {
            // Fenced (recovered) or not-joined: re-admit a fresh peer, like a
            // real service. Only this thread touches peerInstance -> no race.
            privateId = cme::NoDomain;
            peerSlot->peerInstance.reset();
            if (!spawn())
            {
                return;
            }
            peerSlot->ops.fetch_add(1, std::memory_order_relaxed);
        }
        catch (const std::exception&)
        {
            // Expected churn race, usually a control-lock timeout. The domain may still
            // exist and be ours, so keep privateId -- forgetting it here would leak the
            // domain and spawn a second. The loop-top resync drops it if it truly vanished.
            peerSlot->ops.fetch_add(1, std::memory_order_relaxed);
        }
    }
    peerSlot->peerInstance.reset();
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    harness::startLogClock();

    const cme::Strategy strategy = ctx.strategy();
    const char* const stratSuffix = ctx.strategySuffix();

    std::optional<cme::Geometry> region;
    const cme::Geometry::FormatOpts_t fmtOpts{strategy};
    region.emplace(ctx.memory().createRegion(Ceiling, MaxPeers, fmtOpts));

    harness::log("orphan churn: %u workers, %u data slots (%s, backend=%s)", Workers, DataSlots,
                 stratSuffix, ctx.backendName());

    std::array<PeerSlot_t, Workers> peers{};
    for (cme::PeerId i = 0; i < Workers; ++i)
    {
        peers[i].peerId = i;
        peers[i].region = &*region;
        peers[i].coherency = ctx.coherency();
        peers[i].tid = std::thread{worker, &peers[i]};
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
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < end)
    {
        const auto target = pick(rng);
        if (frozen[target])
        {
            peers[target].freezeReq.store(false);
            frozen[target] = false;
            --frozenCount;
        }
        else if (frozenCount + 2 < Workers)
        {
            peers[target].freezeReq.store(true);
            frozen[target] = true;
            ++frozenCount;
        }
        harness::sleepMs(600);
    }

    // ── quiesce: thaw all, idle the op loops, let recovery reclaim every orphan ──
    harness::log("thaw all + settle (recovery drains orphans, fenced peers re-admit)");
    for (cme::PeerId i = 0; i < Workers; ++i)
    {
        peers[i].freezeReq.store(false);
        peers[i].paused.store(true);
    }
    harness::sleepMs(5000);

    // ── leak audit: a fresh peer must still create >= (slots - workers) domains. ──
    std::uint32_t auditCreated = 0;
    {
        cme::Peer audit{*region, cme::admission::claimPeerSlot(*region, ctx.coherency()), ctx.coherency()};
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
        if (peers[i].tid.joinable())
        {
            peers[i].tid.join();
        }
    }

    for (cme::PeerId i = 0; i < Workers; ++i)
    {
        ctx.check(peers[i].ops.load() > 0, "worker made forward progress");
        ctx.check(peers[i].rc.load() == 0, "worker hit no unexpected fatal");
    }
    ctx.check(auditCreated >= DataSlots - Workers,
              "slots not leaked: fresh peer created >= (slots - workers) domains");

    region.reset();
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
