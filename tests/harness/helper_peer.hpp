// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_peer.hpp -- one peer per thread, acquiring in a loop, freezable from outside.
//
// This is the shape every recovery case needs: N peers doing ordinary work, so that freezing one of
// them is a crash the others have to recover from. Four cases wrote the same thread body and the
// same slot, and the copies had drifted only in the freeze idle interval and in what they called
// the stop flag.
//
// Why a thread and not a process: a frozen peer must stop answering while its Peer object stays
// alive, because a destructor would leave membership cleanly and that is the opposite of a crash.
// setFreeze is what stops it, and only the owning thread may touch its own Peer.
//
// The state machine in test_recovery.cpp is deliberately not built on this: it adds a Paused state
// and logs each timeout, and folding those in would leave this loop with a knob per caller.
//
// The struct holds only what the loop below reads or writes. A case needing more derives from it and
// adds its own, which is cheaper than the copy-the-whole-struct that produced these four copies. The
// loop takes a base pointer, so a derived slot needs no virtual and is never sliced.

#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <thread>
#include <utility>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper_util.hpp"
#include "observe/stats.hpp"
#include "test_context.hpp"

namespace harness
{

// What main and the runner share. Every atomic is written by main and read by the runner, except
// the last group, which goes the other way.
struct PeerSlot_t
{
    // ── the worker ─────────────────────────────────────────────────
    std::thread runner;
    // The runner owns it; main asks for a freeze rather than reaching in.
    std::unique_ptr<cme::Peer> peer;

    // ── set before spawnPeerWorker ─────────────────────────────────
    cme::Geometry* region{nullptr};
    cme::PeerId peerId{0};
    cme::CoherencyMode coherency{};
    cme::DomainId domainCount{0};
    // How long a frozen worker idles before it looks at `frozen` again. A case measuring a thaw
    // wants this well under the window it is measuring.
    std::uint32_t idleMs{20};
    // Take one domain and keep it, instead of the acquire/release loop. That loop's guard dies with
    // its iteration, so a freeze aimed at a holder always lands after the release.
    std::atomic<bool> pinned{false};

    // ── main -> runner ─────────────────────────────────────────────
    std::atomic<bool> stop{false};
    // Main raises it; the runner applies it to its own Peer.
    std::atomic<bool> frozen{false};
    // On stop, leak the Peer instead of running its dtor.
    std::atomic<bool> abandon{false};

    // ── runner -> main ─────────────────────────────────────────────
    // Raised once a pinned hold is real.
    std::atomic<bool> holding{false};
    std::atomic<std::uint64_t> acquires{0};
    // The Peer never got built, so this run proves nothing.
    std::atomic<bool> failed{false};
    // The built Peer, for reads only: publishing the pointer is what main lacks when it takes
    // `peer` directly. Null outside the worker's life.
    std::atomic<cme::Peer*> livePeer{nullptr};
};

// One worker's counters, zeroed when its Peer is not up. Check countersLive before believing one:
// without CME_STATS every field is zero because nothing counted.
[[nodiscard]] inline cme::TelemetrySnapshot_t readTelemetry(const PeerSlot_t& slot)
{
    const cme::Peer* peer = slot.livePeer.load(std::memory_order_acquire);
    return peer != nullptr ? peer->getTelemetry() : cme::TelemetrySnapshot_t{};
}

// The thread body: build the Peer, join every data domain, then acquire them round-robin until
// stopped. A lock that times out is counted as nothing and the loop moves on, since under
// contention a timeout is an outcome rather than a failure.
inline void runPeerWorker(PeerSlot_t* slot)
{
    try
    {
        slot->peer = std::make_unique<cme::Peer>(*slot->region, slot->peerId, slot->coherency);
        for (cme::DomainId joinId = 1; joinId <= slot->domainCount; ++joinId)
        {
            slot->peer->joinDomain(joinId);
        }
    }
    catch (const std::exception& error)
    {
        log("peer %u: could not join: %s", slot->peerId, error.what());
        slot->failed.store(true);
        return;
    }
    slot->livePeer.store(slot->peer.get(), std::memory_order_release);

    if (slot->pinned.load(std::memory_order_acquire))
    {
        auto guard = slot->peer->lock(1);
        slot->acquires.fetch_add(1, std::memory_order_relaxed);
        slot->holding.store(true, std::memory_order_release);
        while (!slot->stop.load(std::memory_order_acquire))
        {
            slot->peer->setFreeze(slot->frozen.load(std::memory_order_acquire));
            sleepMs(slot->idleMs);
        }
        // Cleared before the guard goes, so a reader that sees it true knows the hold is still on.
        // That is what lets a case use it as the oracle for "somebody else got in while I held it".
        slot->holding.store(false, std::memory_order_release);
        if (slot->abandon.load(std::memory_order_acquire))
        {
            // Same reasoning as the Peer below: a crashed holder releases nothing, and a guard that
            // runs its destructor hands the domain back before recovery ever sees it held.
            static_cast<void>(new cme::PeerGuard{std::move(guard)});
        }
    }

    cme::DomainId domainId = 1;
    while (!slot->pinned.load(std::memory_order_acquire) &&
           !slot->stop.load(std::memory_order_acquire))
    {
        slot->peer->setFreeze(slot->frozen.load(std::memory_order_acquire));
        if (slot->frozen.load(std::memory_order_acquire))
        {
            sleepMs(slot->idleMs);
            continue;
        }
        try
        {
            const auto guard = slot->peer->lock(domainId);
            slot->acquires.fetch_add(1, std::memory_order_relaxed);
        }
        catch (const cme::LockTimeoutError&)
        {
            // @expected a lock attempt that times out under contention is a normal outcome
        }
        domainId = (domainId % slot->domainCount) + 1;
    }

    slot->livePeer.store(nullptr, std::memory_order_release);  // no reader past this point

    // A peer the case froze is a crashed peer, and a crashed process runs no destructor. Releasing
    // rather than resetting is what keeps its slot dead for recovery to find.
    if (slot->abandon.load(std::memory_order_acquire))
    {
        static_cast<void>(slot->peer.release());
    }
    else
    {
        slot->peer.reset();  // clean leave + destroy
    }
}

// Fill @slot and start its runner. The peer id is the member slot it claims, so a case picks ids
// rather than letting admission choose: freezing "peer 3" has to name one.
//
// idleMs is not a parameter here. A case that wants a different one assigns it before this call,
// which keeps the argument list at what every caller has to say.
inline void spawnPeerWorker(PeerSlot_t& slot, cme::PeerId peerId, cme::Geometry& region,
                            cme::DomainId domainCount)
{
    slot.region = &region;
    slot.peerId = peerId;
    slot.coherency = currentRun().coherency();
    slot.domainCount = domainCount;
    slot.runner = std::thread{runPeerWorker, &slot};
}

// The plural, which is what every multi-peer case actually writes: peer i takes slot i, and the
// group is what joinPeerWorkers and allPeersJoined already take.
//
// idleMs is not a parameter here either. A case wanting a different one assigns it across the
// slots before this call, so the argument list stays at what every caller has to say.
template <typename T_Slots>
void spawnPeerWorkers(T_Slots& slots, std::uint32_t count, cme::Geometry& region,
                      cme::DomainId domainCount)
{
    for (std::uint32_t index = 0; index < count; ++index)
    {
        spawnPeerWorker(slots[index], static_cast<cme::PeerId>(index), region, domainCount);
    }
}

// Stop every worker, then join. Two passes on purpose: raising stop on all of them first lets them
// wind down in parallel rather than one acquire timeout at a time.
template <typename T_Slots>
void joinPeerWorkers(T_Slots& slots, std::uint32_t count)
{
    for (std::uint32_t index = 0; index < count; ++index)
    {
        slots[index].stop.store(true, std::memory_order_release);
    }
    for (std::uint32_t index = 0; index < count; ++index)
    {
        if (slots[index].runner.joinable())
        {
            slots[index].runner.join();
        }
    }
}

// Whether every worker got its Peer built. A false here means the run proved nothing, so a case
// asserts it before reading any other result.
template <typename T_Slots>
[[nodiscard]] bool allPeersJoined(const T_Slots& slots, std::uint32_t count)
{
    for (std::uint32_t index = 0; index < count; ++index)
    {
        if (slots[index].failed.load())
        {
            return false;
        }
    }
    return true;
}

}  // namespace harness
