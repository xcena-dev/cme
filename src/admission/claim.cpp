// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// claim.cpp -- peer-slot claim (Session::open path).
//
// Joins serialise through one nonce lease so peerScanBound has a single writer. With no cross-host
// CAS, a joiner stakes a nonce, settles and re-reads. The joiner whose nonce survived holds the lease.
// A stake flushed more than config::stake::Window after the read that found the lease free or stalled gives up.
// A lease unchanged for config::lease::Timeout is stolen, so a crash mid-claim never wedges later joiners.

#include "admission/claim.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <thread>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "config.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "observe/failpoint.hpp"
#include "util/coherency.hpp"
#include "util/endian.hpp"

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

// Stake a fresh nonce, settle, re-read: the nonce if it survived. The caller rmb'd the line just
// after opening @window, and a stake flushed once @window has closed gives up without settling.
[[nodiscard]] std::optional<std::uint64_t> stakeLease(Geometry::AdmissionControl_t* admissionControl,
                                                      const timing::Deadline& window, CoherencyMode mode)
{
    CME_FAILPOINT_REACH(failpoint::Boundary::AdmissionBeforeStake);
    // Fresh per stake, so a joiner watching a nonce we abandoned sees this stake as a change.
    const std::uint64_t nonce = getRandomNonce();
    admissionControl->nonce = nonce;
    coherency::wmb(admissionControl, sizeof(*admissionControl), mode);
    if (window.expired())
    {
        return std::nullopt;
    }
    std::this_thread::sleep_for(config::stake::Settle);
    if (coherency::get(admissionControl, mode).nonce != nonce)  // rmb + 64B read
    {
        return std::nullopt;
    }
    return nonce;
}

// True if @held stayed unchanged for config::lease::Timeout (holder presumed dead -> steal);
// false if it changed (released / handed off -> recontend).
[[nodiscard]] bool isLeaseStalled(Geometry::AdmissionControl_t* admissionControl, std::uint64_t held,
                                  CoherencyMode mode)
{
    const timing::Deadline until{config::lease::Timeout};
    while (!until.expired())
    {
        std::this_thread::sleep_for(config::lease::PollGap);
        if (coherency::get(admissionControl, mode).nonce != held)  // rmb + 64B read
        {
            return false;
        }
    }
    return true;
}

// Acquire the membership lease (steal-on-stall) and return the nonce that holds it. Throws on
// deadline. A stake that gave up leaves our nonce in the line, and only the steal path clears it.
[[nodiscard]] std::uint64_t acquireLease(Geometry::AdmissionControl_t* admissionControl, CoherencyMode mode)
{
    const timing::Deadline deadline{config::lease::AcquireDeadline};
    while (!deadline.expired())
    {
        const timing::Deadline freeWindow{config::stake::Window};
        const std::uint64_t held = coherency::get(admissionControl, mode).nonce;  // rmb + 64B read

        // (1) Free -> stake it.
        if (held == 0)
        {
            if (const auto nonce = stakeLease(admissionControl, freeWindow, mode))
            {
                return *nonce;
            }
            continue;  // lost the stake race, or staked too late to settle
        }

        // (2) Held -> watch; recontend if the holder frees it.
        if (!isLeaseStalled(admissionControl, held, mode))
        {
            continue;  // holder progressed -> recontend
        }

        // (3) Stalled (holder dead) -> re-confirm, then steal.
        const timing::Deadline stealWindow{config::stake::Window};
        if (coherency::get(admissionControl, mode).nonce != held)  // rmb + 64B read
        {
            continue;  // moved since the watch -> recontend
        }
        if (const auto nonce = stakeLease(admissionControl, stealWindow, mode))
        {
            return *nonce;
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
                member->lastSeenNanos = timing::wall<timing::Nanos>();
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
PeerId claimPeerSlot(const Geometry& geometry)
{
    const CoherencyMode mode = geometry.getMappingCoherency();
    auto* admissionControl = geometry.getAdmissionControl();
    const std::uint64_t nonce = acquireLease(admissionControl, mode);
    CME_FAILPOINT_REACH(failpoint::Boundary::AdmissionBeforeCommit);
    try
    {
        const PeerId peerId = reserveMemberSlot(geometry, mode);
        growPeerScanBound(admissionControl, peerId, mode);
        CME_FAILPOINT_REACH(failpoint::Boundary::AdmissionAfterCommit);
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
