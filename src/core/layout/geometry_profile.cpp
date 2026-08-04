// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// geometry_profile.cpp -- profile slot publish / read, with the build-toggle +
// null + magic gate folded into one internal resolver.

#include "core/layout/geometry_profile.hpp"

#include <cstdint>

#include "cme/shared.hpp"
#include "util/coherency.hpp"
#include "util/time.hpp"

namespace cme
{

void publishProfile(CoherencyMode mode, MemberProfile_t* slot, time::nanoseconds spinTime,
                    time::nanoseconds waitTime) noexcept
{
    if constexpr (!ProfileEnabled)
    {
        (void)slot;
        (void)spinTime;
        (void)waitTime;
        return;
    }
    else
    {
        if (slot == nullptr)
        {
            return;
        }
        auto profile = coherency::get(slot, mode);  // rmb + 64B read
        if (!profile.isValidMagic())
        {
            return;
        }
        // Self is the sole writer of its own profile slot, so a whole-64B set is safe
        // (get-modify-set preserves magic): one wide store beats four field wmb's.
        profile.time.poll = static_cast<std::uint64_t>(time::getThreadCpuTime().count());
        profile.time.worker = spinTime;  // worker on-disk publishes spin (no DRAM accumulator)
        profile.time.wait = waitTime;
        profile.time.spin = spinTime;
        coherency::set(slot, profile, mode);  // 64B store + wmb
    }
}

ProfileTimes_t readProfile(MemberProfile_t* slot, CoherencyMode mode) noexcept
{
    ProfileTimes_t out{};
    if constexpr (ProfileEnabled)
    {
        if (slot != nullptr)
        {
            const auto profile = coherency::get(slot, mode);  // rmb + 64B read
            if (profile.isValidMagic())
            {
                out.poll = profile.time.poll;
                out.worker = profile.time.worker;
                out.wait = profile.time.wait;
                out.spin = profile.time.spin;
            }
        }
    }
    return out;
}

}  // namespace cme
