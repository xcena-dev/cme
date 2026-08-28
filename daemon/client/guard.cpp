// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// client/guard.cpp -- see cmed/guard.hpp. The guard's whole job is giving the turn back: publish
// Idle, read who is queued while the mutex still makes that answer stable, release the mutex, then
// ring the daemon only when nobody local queued for the turn.

#include "cmed/guard.hpp"

#include <atomic>
#include <cstdint>
#include <utility>

#include "cmed/robust_lock.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed
{

CmedGuard::CmedGuard(protocol::SharedArea_t& area, std::atomic<std::uint32_t>& holders, std::uint32_t domainId,
                     util::RobustLock held) noexcept
    : area_{&area},
      holders_{&holders},
      held_{std::move(held)},
      domainId_{domainId}
{
}

CmedGuard::CmedGuard(CmedGuard&& other) noexcept
    : area_{std::exchange(other.area_, nullptr)},
      holders_{other.holders_},
      held_{std::move(other.held_)},
      domainId_{other.domainId_}
{
}

CmedGuard& CmedGuard::operator=(CmedGuard&& other) noexcept
{
    if (this != &other)
    {
        unlock();

        area_ = std::exchange(other.area_, nullptr);
        holders_ = other.holders_;
        held_ = std::move(other.held_);
        domainId_ = other.domainId_;
    }
    return *this;
}

CmedGuard::~CmedGuard() noexcept
{
    unlock();
}

void CmedGuard::unlock() noexcept
{
    if (area_ != nullptr)
    {
        release();
        area_ = nullptr;

        // Last, so the session cannot unmap the area while the release above is still writing it.
        holders_->fetch_sub(1, std::memory_order_release);
    }
}

void CmedGuard::release() noexcept
{
    auto& context = area_->domain.table[domainId_];

    // Ordered against the waiters read below: the daemon stores validity then reads this word,
    // so leaving Idle in this core's buffer would let a hand-back race a section that rode the turn.
    context.publish(protocol::RequestState::Idle);

    // A local requester queued to use this turn next leaves the daemon nothing to decide, so it is
    // told nothing. Read here, under the mutex, which is what makes the answer stable.
    const bool nobodyQueued = !context.hasWaiters();

    // The mutex first, so a daemon woken below finds it free. Ringing under it wakes a pass that can
    // only block on the word this line is about to release.
    held_.unlock();

    // Not waited for: the answer changes nothing this caller does next.
    if (nobodyQueued)
    {
        area_->ring(domainId_);
    }
}

bool CmedGuard::wasAbandoned() const noexcept
{
    return area_ != nullptr && held_.wasAbandoned();
}

std::uint16_t CmedGuard::readAbandonCount() const noexcept
{
    if (area_ == nullptr)
    {
        return 0;
    }
    return area_->domain.table[domainId_].getAbandonCount();
}

bool CmedGuard::clearAbandonCount(std::uint16_t seen) noexcept
{
    return area_ != nullptr && area_->domain.table[domainId_].clearAbandonCount(seen);
}

bool CmedGuard::stillHolds() const noexcept
{
    if (area_ == nullptr)
    {
        return false;
    }

    return area_->domain.table[domainId_].hasValidTurn();
}

}  // namespace cmed
