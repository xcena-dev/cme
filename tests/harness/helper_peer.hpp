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

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "helper_util.hpp"
#include "test_context.hpp"

namespace harness
{

// What main and the runner share. Every atomic is written by main and read by the runner, except
// `acquires` and `failed`, which go the other way.
struct PeerSlot_t
{
    std::thread runner;
    std::unique_ptr<cme::Peer> peer;  // the runner owns it; main asks for a freeze rather than reaching in
    cme::Geometry* region{nullptr};
    cme::PeerId peerId{0};
    cme::CoherencyMode coherency{};
    cme::DomainId domainCount{0};
    // How long a frozen worker idles before it looks at `frozen` again. A case measuring how fast a
    // thaw takes effect wants this well under the window it is measuring.
    std::uint32_t idleMs{20};
    std::atomic<bool> stop{false};
    std::atomic<bool> frozen{false};   // main raises it; the runner applies it to its own Peer
    std::atomic<bool> abandon{false};  // on stop, leak the Peer instead of running its dtor
    std::atomic<std::uint64_t> acquires{0};
    std::atomic<bool> failed{false};  // the Peer never got built, so this run proves nothing
};

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

    cme::DomainId domainId = 1;
    while (!slot->stop.load(std::memory_order_acquire))
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
