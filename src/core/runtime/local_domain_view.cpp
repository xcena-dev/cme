// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// local_domain_view.cpp -- LocalDomainView method implementations.

#include "core/runtime/local_domain_view.hpp"

#include <cstdint>

#include "common/timing.hpp"
#include "config.hpp"

namespace cme
{

void LocalDomainView::pinOwnership() noexcept
{
    // CAS loop instead of fetch_add: a pin must not land while a transfer publish
    // holds the word (TransferringPins) -- spin the latch out (publish-length) so
    // the subsequent record validation sees the post-transfer holder.
    auto cur = pin_.count.load();
    for (;;)
    {
        if (cur == TransferringPins)
        {
            cur = pin_.count.load();
            continue;
        }
        if (pin_.count.cas(cur, cur + 1))
        {
            break;
        }
    }
    // First pin records the start timestamp; nested pins inherit it.
    if (cur == 0)
    {
        pin_.startTimestamp = timing::now();
    }
}

void LocalDomainView::unpinOwnership() noexcept
{
    auto cur = pin_.count.load();
    while (cur > 0 && cur != TransferringPins)  // sentinel unreachable here (pins block it); guard anyway
    {
        if (pin_.count.cas(cur, cur - 1))
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
    return pin_.count.cas(expected, TransferringPins);
}

void LocalDomainView::endOwnershipTransfer() noexcept
{
    pin_.count.store(0);
}

void LocalDomainView::resetOwnershipPins() noexcept
{
    pin_.count.reset(0);
}

bool LocalDomainView::canTransferOwnership() const noexcept
{
    return pin_.count.load() == 0;
}

bool LocalDomainView::hasOwnershipPins() const noexcept
{
    // The transfer sentinel is the poll thread's latch, not a worker's pin.
    const auto pins = pin_.count.load();
    return pins != 0 && pins != TransferringPins;
}

std::uint32_t LocalDomainView::getOwnershipPinCount() const noexcept
{
    return pin_.count.load();
}

timing::TimePoint LocalDomainView::getOwnershipPinStart() const noexcept
{
    return pin_.startTimestamp;
}

void LocalDomainView::becomeHolder() noexcept
{
    ownership_.isHolder.store(true);
}

void LocalDomainView::loseHolder() noexcept
{
    ownership_.isHolder.store(false);
}

bool LocalDomainView::isHolder() const noexcept
{
    return ownership_.isHolder.load();
}

bool LocalDomainView::isTurnToCheckOwnership(timing::TimePoint now) noexcept
{
    // Poll thread only.
    return poll_.ownershipCheck.isDue(now, config::poll_thread::OwnershipCheckInterval);
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
