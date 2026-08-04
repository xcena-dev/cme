// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// endian.hpp -- tear-free LE load/store for region-resident scalars.
// Relaxed atomic = READ_ONCE/WRITE_ONCE; cross-host ordering via coherency::wmb/rmb.

#pragma once

#include <atomic>
#include <type_traits>

namespace cme::endian
{

namespace detail
{

// CPU-native <-> LE: identity on LE, __builtin_bswap on BE.
template <typename T>
[[nodiscard]] constexpr T adjustEndian(T value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "endian helpers require trivially-copyable scalar");
    static_assert(sizeof(T) <= 8, "endian helpers expect <=8 B scalar");
    // clang-format off
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    if constexpr (sizeof(T) == 1)       return value;
    else if constexpr (sizeof(T) == 2)  return __builtin_bswap16(value);
    else if constexpr (sizeof(T) == 4)  return __builtin_bswap32(value);
    else                                return __builtin_bswap64(value);
#endif
    // clang-format on
}

}  // namespace detail

template <typename T>
[[nodiscard]] inline T load(const T& field) noexcept
{
    const auto* slot = reinterpret_cast<const std::atomic<T>*>(&field);
    return detail::adjustEndian(slot->load(std::memory_order_relaxed));
}

template <typename T>
inline void store(T& field, T value) noexcept
{
    auto* slot = reinterpret_cast<std::atomic<T>*>(&field);
    slot->store(detail::adjustEndian(value), std::memory_order_relaxed);
}

// ── endian::Field_t<T> ───────────────────────────────────────────────
// Scalar wrapper for region-resident fields. Implicit conversion through load/store.
// sizeof(Field_t<T>) == sizeof(T); `.raw` exposes the encoded bytes.
template <typename T>
struct Field_t
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "endian::Field_t requires trivially-copyable scalar");
    static_assert(sizeof(T) <= 8, "endian::Field_t expects <=8 B scalar");

    T raw;

    Field_t() = default;
    Field_t(const Field_t&) = default;
    Field_t& operator=(const Field_t&) = default;

    [[nodiscard]] operator T() const noexcept
    {
        return load(raw);
    }

    Field_t& operator=(T value) noexcept
    {
        store(raw, value);
        return *this;
    }
};

}  // namespace cme::endian
