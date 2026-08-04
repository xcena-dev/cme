// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// claim.cpp -- peer-slot claim (Session::open path).
//
// Joins serialise through one nonce lease so peerScanBound has a single writer.
// Atomic-free (no cross-host CAS): stake a nonce, settle, re-read; the surviving
// writer holds it. A lease unchanged for LeaseTimeout is stolen, so a crash
// mid-claim never wedges later joiners.

#include "admission/claim.hpp"

#include <chrono>
#include <cstdint>
#include <random>
#include <thread>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "config.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "util/coherency.hpp"
#include "util/endian.hpp"
#include "util/time.hpp"

namespace cme::admission
{

namespace
{

// Lease token (not crypto). Retry until nonzero: nonce==0 is the unlocked sentinel.
std::uint64_t getRandomNonce()
{
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    std::uint64_t nonce{0};
    while (nonce == 0)
    {
        nonce = engine();
    }
    return nonce;
}

// Stake @nonce, settle, re-read: true if it survived (we hold the lease). Caller
// must have just rmb'd @admissionControl so the flushed-back peerScanBound is fresh.
[[nodiscard]] bool stakeLease(Geometry::AdmissionControl_t* admissionControl, std::uint64_t nonce,
                              CoherencyMode mode)
{
    admissionControl->nonce = nonce;
    coherency::wmb(admissionControl, sizeof(*admissionControl), mode);
    std::this_thread::sleep_for(ClaimSettle);
    return coherency::get(admissionControl, mode).nonce == nonce;  // rmb + 64B read
}

// True if @held stayed unchanged for LeaseTimeout (holder presumed dead -> steal);
// false if it changed (released / handed off -> recontend).
[[nodiscard]] bool isLeaseStalled(Geometry::AdmissionControl_t* admissionControl, std::uint64_t held,
                                  CoherencyMode mode)
{
    const auto until = std::chrono::steady_clock::now() + LeaseTimeout;
    while (std::chrono::steady_clock::now() < until)
    {
        std::this_thread::sleep_for(LeasePollGap);
        if (coherency::get(admissionControl, mode).nonce != held)  // rmb + 64B read
        {
            return false;
        }
    }
    return true;
}

// Acquire the membership lease (steal-on-stall). Throws on deadline.
void acquireLease(Geometry::AdmissionControl_t* admissionControl, std::uint64_t nonce,
                  CoherencyMode mode)
{
    const auto deadline = std::chrono::steady_clock::now() + LeaseAcquireDeadline;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const std::uint64_t held = coherency::get(admissionControl, mode).nonce;  // rmb + 64B read

        // (1) Free -> stake it.
        if (held == 0)
        {
            if (stakeLease(admissionControl, nonce, mode))
            {
                return;
            }
            continue;  // lost the stake race
        }

        // (2) Held -> watch; recontend if the holder frees it.
        if (!isLeaseStalled(admissionControl, held, mode))
        {
            continue;  // holder progressed -> recontend
        }

        // (3) Stalled (holder dead) -> re-confirm, then steal.
        if (coherency::get(admissionControl, mode).nonce == held && stakeLease(admissionControl, nonce, mode))  // rmb + 64B read
        {
            return;
        }
    }
    throw NoFreeSlotError{"cme: membership lease not acquired before deadline"};
}

void releaseLease(Geometry::AdmissionControl_t* admissionControl, std::uint64_t nonce,
                  CoherencyMode mode) noexcept
{
    coherency::rmwIfTrue(
        admissionControl, mode,
        [nonce](auto* control)
        {
            if (control->nonce != nonce)
            {
                return false;
            }
            control->nonce = 0u;
            return true;
        });
}

// Caller holds the lease. Reserve the lowest free slot: Active marks it so a concurrent
// allocator skips it, and lastSeenNanos seeds the liveness witness until joinMembership.
[[nodiscard]] PeerId reserveMemberSlot(const Geometry& geometry, CoherencyMode mode)
{
    const std::uint32_t peerCount = geometry.getPeerCount();
    for (const PeerId peerId : IdRange<PeerId>(PeerId{0}, PeerId{peerCount}))
    {
        const bool success = coherency::rmwIfTrue(
            geometry.getMemberSlot(peerId), mode,
            [&](auto* member)
            {
                if (!member->isValidMagic() || !member->hasStatus(Geometry::Member_t::Status::None))
                {
                    return false;  // bad slot or already taken
                }
                member->setStatus(Geometry::Member_t::Status::Active);
                member->lastSeenNanos = time::clockNowNanos();
                return true;
            });
        if (success)
        {
            return peerId;
        }
    }
    throw NoFreeSlotError{"cme: no free peer slot"};
}

// Grow peerScanBound to cover @peerId so membership scans reach the new slot. Monotone;
// the caller's lease makes it the sole writer.
void growPeerScanBound(Geometry::AdmissionControl_t* admissionControl, PeerId peerId,
                       CoherencyMode mode)
{
    const std::uint32_t bound = static_cast<std::uint32_t>(peerId) + 1;
    if (bound > admissionControl->peerScanBound)
    {
        admissionControl->peerScanBound = bound;
        coherency::wmb(admissionControl, sizeof(*admissionControl), mode);
    }
}

}  // namespace

// Hold the lease, reserve the lowest free slot, release. The lease is always
// released -- including on a full-table throw -- so a failed claim never wedges.
PeerId claimPeerSlot(const Geometry& geometry, CoherencyMode mode)
{
    auto* admissionControl = geometry.getAdmissionControl();
    const std::uint64_t nonce = getRandomNonce();

    acquireLease(admissionControl, nonce, mode);
    try
    {
        const PeerId peerId = reserveMemberSlot(geometry, mode);
        growPeerScanBound(admissionControl, peerId, mode);
        releaseLease(admissionControl, nonce, mode);
        return peerId;
    }
    catch (...)
    {
        releaseLease(admissionControl, nonce, mode);
        throw;
    }
}

}  // namespace cme::admission
