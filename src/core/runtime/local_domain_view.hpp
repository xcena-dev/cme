// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// local_domain_view.hpp -- per-peer per-domain local DRAM state: ownership
// belief, Use-phase pin, poll/worker sequencing baselines. NOT the
// authoritative shared record (Geometry::DomainRecord_t).

#pragma once

#include <atomic>
#include <cstdint>

#include "util/time.hpp"

namespace cme
{

class LocalDomainView
{
public:
    // ── Ownership pin (worker ↔ pollThread transfer barrier) ─────
    // pollThread will not forward while ownershipPins > 0, so a worker's Use phase cannot be
    // cut short. That covers worker vs pollThread only: two workers of this peer are not
    // serialised anywhere, since the domain lock is a per-peer token they both pass by
    // residency. cme::SharedSession is the shipped fix: a per-domain mutex above this token.
    void pinOwnership() noexcept;
    void unpinOwnership() noexcept;
    void resetOwnershipPins() noexcept;
    [[nodiscard]] bool canTransferOwnership() const noexcept;
    [[nodiscard]] std::uint32_t getOwnershipPinCount() const noexcept;
    [[nodiscard]] time::TimePoint getOwnershipPinStart() const noexcept;

    // ── Transfer latch (closes the grant-vs-resident TOCTOU) ─────
    // CAS pin 0 -> Transferring makes "no worker pinned" and "block new pins" one atomic
    // step, so no resident fast-path can validate the record mid-publish.
    [[nodiscard]] bool tryBeginOwnershipTransfer() noexcept;
    void endOwnershipTransfer() noexcept;

    // ── Ownership-holder cache ────────────────────────────────────
    void becomeHolder() noexcept;
    void loseHolder() noexcept;
    [[nodiscard]] bool isHolder() const noexcept;

    // ── Poll-thread sequencing ────────────────────────────────────
    [[nodiscard]] bool isTurnToCheckOwnership() noexcept;

    // ── Worker sequencing (phantom-filter baselines) ─────────────
    [[nodiscard]] std::uint64_t getLastOwnershipEpoch() const noexcept;
    void setLastOwnershipEpoch(std::uint64_t ownershipEpoch) noexcept;

private:
    // Pin-word sentinel: a transfer publish holds the word (see tryBeginOwnershipTransfer).
    static constexpr std::uint32_t TransferringPins = UINT32_MAX;

    struct
    {
        std::atomic<std::uint32_t> count{0};
        time::TimePoint startTimestamp{};
    } pin_;

    struct
    {
        std::atomic<bool> isHolder{false};
    } ownership_;

    struct
    {
        std::uint32_t ownershipCheckCountdown{0};  // poll-only: cycles until next record check
    } poll_;

    struct
    {
        std::uint64_t lastOwnershipEpoch{0};
    } worker_;
};

}  // namespace cme
