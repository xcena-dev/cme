// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// geometry_profile.hpp -- MemberProfile_t on-disk line + profile slot I/O.
//
// Profile section sits at region tail so CME_PROFILE toggle never shifts a protocol section.
// publishProfile / readProfile fold build-toggle + null + magic checks; callers need no guard.

#pragma once

#include <cstdint>

#include "cme/shared.hpp"
#include "core/types.hpp"
#include "util/endian.hpp"

namespace cme
{

// CME_PROFILE off: profile section omitted (size 0). ON/OFF builds cannot share a formatted region.
#if defined(CME_PROFILE)
inline constexpr bool ProfileEnabled = true;
#else
inline constexpr bool ProfileEnabled = false;
#endif

// Per-peer observability cacheline, separate from Member_t so hot membership
// writes don't dirty profile bytes. Writer = peer indexed (Inv1 by construction).
struct MemberProfile_t
{
    static constexpr std::uint32_t Magic = 0x434D4550u;  // "CMEP"

    endian::Field_t<std::uint32_t> magic;  //  0: MemberProfile_t::Magic
    std::uint32_t pad;                     //  4
    struct
    {
        endian::Field_t<NanosCount> poll;    //  8: poll-thread CPU
        endian::Field_t<NanosCount> worker;  // 16: worker CPU during wait
        endian::Field_t<NanosCount> wait;    // 24: ownership-wait wall
        endian::Field_t<NanosCount> spin;    // 32: ownership-wait CPU (busy-spin proxy)
    } time;
    std::uint8_t reserved[24];  // 40..63: pad to 64B

    [[nodiscard]] bool isValidMagic() const noexcept
    {
        return static_cast<std::uint32_t>(magic) == Magic;
    }
};

static_assert(sizeof(MemberProfile_t) == 64, "MemberProfile_t must be 64 B");

// Cumulative per-peer CPU/wait/spin counters in host endianness; the read-side
// view handed to inspectors so they never touch on-disk endian fields directly.
struct ProfileTimes_t
{
    std::uint64_t poll{0};    // poll-thread CPU
    std::uint64_t worker{0};  // worker CPU during wait
    std::uint64_t wait{0};    // ownership-wait wall
    std::uint64_t spin{0};    // ownership-wait CPU (busy-spin proxy)
};

// Bytes for the profile section tail. Returns 0 when CME_PROFILE is off.
[[nodiscard]] inline constexpr std::uint64_t
getProfileAreaSize(std::uint32_t /*domainCount*/, std::uint32_t peerCount) noexcept
{
    if constexpr (ProfileEnabled)
    {
        return static_cast<std::uint64_t>(peerCount) * sizeof(MemberProfile_t);
    }
    else
    {
        (void)peerCount;
        return 0;
    }
}

// Publish this peer's wait/spin/CPU counters to its profile cacheline.
// No-op when profiling is built out or slot is absent/unformatted.
void publishProfile(CoherencyMode mode, MemberProfile_t* slot, NanosCount spinTime,
                    NanosCount waitTime) noexcept;

// Snapshot this peer's counters. Returns all-zero when profiling is off or slot invalid.
[[nodiscard]] ProfileTimes_t readProfile(MemberProfile_t* slot, CoherencyMode mode) noexcept;

}  // namespace cme
