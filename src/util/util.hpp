// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// util.hpp -- small cross-cutting helpers (waitUntil, Passkey).
// CXL fences -> coherency.hpp; byte order -> endian.hpp.

#pragma once

#include <thread>

#include "common/timing.hpp"

namespace cme
{

// ── ceilDiv ────────────────────────────────────────────────────────
// Ceiling of numerator/denominator for unsigned integers (denominator > 0).
template <typename T>
[[nodiscard]] constexpr T ceilDiv(T numerator, T denominator) noexcept
{
    return (numerator + denominator - 1) / denominator;
}

// ── roundUp ────────────────────────────────────────────────────────
// Round value up to a multiple of align (a power of two). Zero rounds up to align.
template <typename T>
[[nodiscard]] constexpr T roundUp(T value, T align) noexcept
{
    return value == 0 ? align : ((value + align - 1) & ~(align - 1));
}

// ── waitUntil ──────────────────────────────────────────────────────
// Poll pred() every interval; true on success, false on timeout. interval==0: spin.
template <typename T_Predicate>
[[nodiscard]] bool waitUntil(T_Predicate pred, timing::Millis timeout,
                             timing::Millis interval = timing::Millis{10})
{
    const timing::Deadline deadline{timeout};
    while (!pred())
    {
        if (deadline.expired())
        {
            return false;
        }
        if (interval > timing::Millis::zero())
        {
            std::this_thread::sleep_for(interval);
        }
    }
    return true;
}

// ── Passkey<T_Holder> ────────────────────────────────────────────────
// Compile-time access token: methods taking Passkey<X> are callable only from X.
template <typename T_Holder>
class Passkey
{
private:
    friend T_Holder;
    Passkey() = default;

public:
    Passkey(const Passkey&) = default;
};

}  // namespace cme
