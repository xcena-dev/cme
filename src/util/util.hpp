// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// util.hpp -- small cross-cutting helpers (integer rounding, Passkey).
// CXL fences -> coherency.hpp; byte order -> endian.hpp; polling waits -> common/poll.hpp.

#pragma once

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
