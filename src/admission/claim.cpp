// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// claim.cpp -- peer-slot claim (Session::open path).
//
// Joins serialise through one nonce lease so peerScanBound has a single writer. The lease is a
// stake::Claim over a random nonce per attempt: staked, settled and re-read, held and withdrawn when
// late. A nonce unchanged for config::lease::Timeout is stolen, so a crash mid-claim never wedges joiners.

#include "admission/claim.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <thread>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "config.hpp"
#include "core/algo/stake_claim.hpp"
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

// The lease holder's token, zero while the lease is free.
using NonceField = stake::FieldWord<Geometry::AdmissionControl_t, std::uint64_t, &Geometry::AdmissionControl_t::nonce>;

// Clears the line only while it still carries @nonce, so another joiner's stake is never erased.
void releaseLease(Geometry::AdmissionControl_t* admissionControl, std::uint64_t nonce,
                  CoherencyMode mode) noexcept
{
    NonceField{admissionControl, mode, 0}.retract(nonce, failpoint::Boundary::AdmissionReleaseAfterCheck);
}

// The lease line as the claim procedure sees it: a fresh random nonce per attempt, free at zero,
// and open to a steal once one nonce has stood for config::lease::Timeout.
class LeaseWord
{
public:
    using Value = std::uint64_t;

    // A late stake may have overwritten a holder mid-claim, so it stays until that holder's last write lands.
    static constexpr auto LateHold = config::lease::LateStakeHold;
    static constexpr std::optional<timing::Millis> StallTimeout{config::lease::Timeout};

    LeaseWord(Geometry::AdmissionControl_t* admissionControl, CoherencyMode mode) noexcept
        : field_{admissionControl, mode, 0},
          mode_{mode}
    {
    }

    [[nodiscard]] Value read() const
    {
        return field_.read();
    }

    [[nodiscard]] static bool isFree(Value nonce) noexcept
    {
        return nonce == 0;
    }

    // Fresh per stake, so a joiner watching a nonce we abandoned sees this stake as a change.
    [[nodiscard]] static Value makeOwn()
    {
        return getRandomNonce();
    }

    // Only the nonce is stored, not a copy of the line: the holder writes peerScanBound in this line
    // too, and writing back an older copy would undo it.
    [[nodiscard]] bool publish(Value nonce) noexcept
    {
        CME_FAILPOINT_REACH(failpoint::Boundary::AdmissionBeforeStake);
        auto* const control = field_.getLine();
        control->nonce = nonce;
        coherency::wmb(control, sizeof(*control), mode_);
        return true;
    }

    void retract(Value nonce) noexcept
    {
        CME_FAILPOINT_REACH(failpoint::Boundary::AdmissionBeforeWithdraw);
        releaseLease(field_.getLine(), nonce, mode_);
    }

private:
    NonceField field_;
    CoherencyMode mode_;
};

// Acquire the membership lease and return the nonce that holds it. Throws on deadline. A stake that
// gave up has cleared its own nonce, unless it died first and left it to a steal.
[[nodiscard]] std::uint64_t acquireLease(Geometry::AdmissionControl_t* admissionControl, CoherencyMode mode)
{
    LeaseWord word{admissionControl, mode};
    stake::Claim<LeaseWord::Value> claim;
    const timing::Deadline deadline{config::lease::AcquireDeadline};
    while (!deadline.expired())
    {
        if (claim.tick(word) == stake::Outcome::Won)
        {
            return claim.getMine();
        }
        std::this_thread::sleep_for(claim.getNextWait(config::lease::PollGap));
    }
    throw NoFreeSlotError{"cme: membership lease not acquired before deadline"};
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
