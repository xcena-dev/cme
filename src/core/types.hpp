// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// internal/types.hpp -- private identifier types + numeric limits.

#pragma once

#include <cstdint>
#include <limits>

namespace cme
{

// Peer id, 0..MaxPeers-1, identical to the on-disk slot index. NoPeer
// (UINT32_MAX) is the sentinel for "no peer".
using PeerId = std::uint32_t;

// Domain index, 0..NumDomains-1. Each domain is an independent lock.
using DomainId = std::uint32_t;

// A nanosecond count as a shared 64-bit field holds it, which is what a stored counter and an
// endian::Field_t need. Not a duration: timing::Nanos is that, and it does not fit an atomic.
using NanosCount = std::uint64_t;

// Slot 0: internal control domain (serialises create/deleteDomain). Not user-addressable.
inline constexpr DomainId ControlDomainId = 0;
[[nodiscard]] inline constexpr bool isControlDomain(DomainId domainId) noexcept
{
    return domainId == ControlDomainId;
}

// Sentinel: name lookup missed or no free slot.
inline constexpr DomainId NoDomain = std::numeric_limits<DomainId>::max();
[[nodiscard]] inline constexpr bool isNoDomain(DomainId domainId) noexcept
{
    return domainId == NoDomain;
}

// Sentinel: no owner (uninitialized or post-format).
inline constexpr PeerId NoPeer = std::numeric_limits<PeerId>::max();
[[nodiscard]] inline constexpr bool isNoPeer(PeerId peerId) noexcept
{
    return peerId == NoPeer;
}

// Hardcoded ceilings; mirror me_layout.h's enum me_limits.
inline constexpr std::uint32_t MaxPeers = 64;
inline constexpr std::uint32_t MaxDomains = 64;

// Bound-checked predicates for layout dimension parameters.
[[nodiscard]] inline constexpr bool isValidDomainCount(std::uint32_t count) noexcept
{
    return count > 0 && count <= MaxDomains;
}
[[nodiscard]] inline constexpr bool isValidPeerCount(std::uint32_t count) noexcept
{
    return count > 0 && count <= MaxPeers;
}

// Half-open [begin, end) so ids with no backing container still walk with a range-based for.
// std::views::iota is C++20 and we target C++17; this lowers to a plain index loop.
template <typename T_Id>
class IdRange
{
public:
    constexpr IdRange(T_Id begin, T_Id end) noexcept
        : begin_{begin},
          end_{end}
    {
    }

    class iterator
    {
    public:
        constexpr explicit iterator(T_Id value) noexcept
            : value_{value}
        {
        }
        [[nodiscard]] constexpr T_Id operator*() const noexcept
        {
            return value_;
        }
        constexpr iterator& operator++() noexcept
        {
            ++value_;
            return *this;
        }
        [[nodiscard]] constexpr bool operator!=(iterator other) const noexcept
        {
            return value_ != other.value_;
        }

    private:
        T_Id value_;
    };

    [[nodiscard]] constexpr iterator begin() const noexcept
    {
        return iterator{begin_};
    }
    [[nodiscard]] constexpr iterator end() const noexcept
    {
        return iterator{end_};
    }

private:
    T_Id begin_;
    T_Id end_;
};

// IdRange but modular: from @start, wrapping at @bound, yielding the other bound-1 ids in
// successor order and excluding @start. Bound by the live extent to skip empty high slots.
template <typename T_Id>
class IdRing
{
public:
    constexpr IdRing(T_Id start, T_Id bound) noexcept
        : start_{start},
          bound_{bound}
    {
    }

    class iterator
    {
    public:
        constexpr iterator(T_Id start, T_Id bound, T_Id step) noexcept
            : start_{start},
              bound_{bound},
              step_{step}
        {
        }
        [[nodiscard]] constexpr T_Id operator*() const noexcept
        {
            return static_cast<T_Id>((start_ + step_) % bound_);
        }
        constexpr iterator& operator++() noexcept
        {
            ++step_;
            return *this;
        }
        [[nodiscard]] constexpr bool operator!=(iterator other) const noexcept
        {
            return step_ != other.step_;
        }

    private:
        T_Id start_;
        T_Id bound_;
        T_Id step_;
    };

    [[nodiscard]] constexpr iterator begin() const noexcept
    {
        return iterator{start_, bound_, T_Id{1}};
    }
    [[nodiscard]] constexpr iterator end() const noexcept
    {
        return iterator{start_, bound_, bound_};
    }

private:
    T_Id start_;
    T_Id bound_;
};

// Outcome of an ownership-wait. Peer::lock translates NotArrived -> LockTimeoutError.
enum class OwnershipResult
{
    Arrived,
    NotArrived,
};

}  // namespace cme
