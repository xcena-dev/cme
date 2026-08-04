// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// local_domain_view.cpp -- LocalDomainView method implementations.

#include "core/runtime/local_domain_view.hpp"

#include <atomic>
#include <cstdint>

#include "config.hpp"
#include "util/time.hpp"

namespace cme
{

void LocalDomainView::pinOwnership() noexcept
{
    // CAS loop instead of fetch_add: a pin must not land while a transfer publish
    // holds the word (TransferringPins) -- spin the latch out (publish-length) so
    // the subsequent record validation sees the post-transfer holder.
    auto cur = pin_.count.load(std::memory_order_acquire);
    for (;;)
    {
        if (cur == TransferringPins)
        {
            cur = pin_.count.load(std::memory_order_acquire);
            continue;
        }
        if (pin_.count.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel))
        {
            break;
        }
    }
    // First pin records the start timestamp; nested pins inherit it.
    if (cur == 0)
    {
        pin_.startTimestamp = time::getMonoTime();
    }
}

void LocalDomainView::unpinOwnership() noexcept
{
    auto cur = pin_.count.load(std::memory_order_relaxed);
    while (cur > 0 && cur != TransferringPins)  // sentinel unreachable here (pins block it); guard anyway
    {
        if (pin_.count.compare_exchange_weak(cur, cur - 1,
                                             std::memory_order_relaxed))
        {
            return;
        }
    }
}

bool LocalDomainView::tryBeginOwnershipTransfer() noexcept
{
    // One atomic step: succeed only when no worker holds a pin, and block new pins
    // until endOwnershipTransfer. Failure = a worker re-pinned; caller keeps the domain.
    std::uint32_t expected = 0;
    return pin_.count.compare_exchange_strong(expected, TransferringPins,
                                              std::memory_order_acq_rel);
}

void LocalDomainView::endOwnershipTransfer() noexcept
{
    pin_.count.store(0, std::memory_order_release);
}

void LocalDomainView::resetOwnershipPins() noexcept
{
    pin_.count.store(0, std::memory_order_relaxed);
}

bool LocalDomainView::canTransferOwnership() const noexcept
{
    return pin_.count.load(std::memory_order_acquire) == 0;
}

std::uint32_t LocalDomainView::getOwnershipPinCount() const noexcept
{
    return pin_.count.load(std::memory_order_acquire);
}

time::TimePoint LocalDomainView::getOwnershipPinStart() const noexcept
{
    return pin_.startTimestamp;
}

void LocalDomainView::becomeHolder() noexcept
{
    ownership_.isHolder.store(true, std::memory_order_release);
}

void LocalDomainView::loseHolder() noexcept
{
    ownership_.isHolder.store(false, std::memory_order_relaxed);
}

bool LocalDomainView::isHolder() const noexcept
{
    return ownership_.isHolder.load(std::memory_order_acquire);
}

bool LocalDomainView::isTurnToCheckOwnership() noexcept
{
    // True once every OwnershipCheckInterval cycles, then rearms. Poll thread only.
    if (poll_.ownershipCheckCountdown == 0)
    {
        poll_.ownershipCheckCountdown = OwnershipCheckInterval - 1;
        return true;
    }
    --poll_.ownershipCheckCountdown;
    return false;
}

std::uint64_t LocalDomainView::getLastOwnershipEpoch() const noexcept
{
    return worker_.lastOwnershipEpoch;
}

void LocalDomainView::setLastOwnershipEpoch(std::uint64_t ownershipEpoch) noexcept
{
    worker_.lastOwnershipEpoch = ownershipEpoch;
}

}  // namespace cme
