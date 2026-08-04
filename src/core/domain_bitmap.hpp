// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// domain_bitmap.hpp -- packed set of domain ids + word-count constants.
// The uint64-word backing store for the on-disk participation / demand bitmaps: load a
// slot's words into a DomainBitmap to operate, store the words back to publish.

#pragma once

#include <cstdint>

#include "core/types.hpp"
#include "util/util.hpp"

namespace cme
{

// Bits in one bitmap word (the backing word type is uint64).
inline constexpr std::uint32_t DomainBitsPerWord = 64;

// Words needed for all domains; sizes both in-memory bitmap and on-disk array.
inline constexpr std::uint32_t DomainWordCount = ceilDiv(MaxDomains, DomainBitsPerWord);

class DomainBitmap
{
public:
    constexpr DomainBitmap() noexcept = default;

    // Raw word access for on-disk load/store.
    constexpr void setWord(std::uint32_t index, std::uint64_t value) noexcept
    {
        words_[index] = value;
    }
    [[nodiscard]] constexpr std::uint64_t getWord(std::uint32_t index) const noexcept
    {
        return words_[index];
    }
    // Contiguous word array for bulk memcpy load/store (host-endian; low word =
    // low domain ids). Spans DomainWordCount words.
    [[nodiscard]] constexpr std::uint64_t* getData() noexcept
    {
        return words_;
    }
    [[nodiscard]] constexpr const std::uint64_t* getData() const noexcept
    {
        return words_;
    }

    constexpr void set(DomainId domain) noexcept
    {
        words_[domain / DomainBitsPerWord] |= bitMask(domain);
    }
    constexpr void clear(DomainId domain) noexcept
    {
        words_[domain / DomainBitsPerWord] &= ~bitMask(domain);
    }
    [[nodiscard]] constexpr bool has(DomainId domain) const noexcept
    {
        return (words_[domain / DomainBitsPerWord] & bitMask(domain)) != 0;
    }
    [[nodiscard]] constexpr bool isEmpty() const noexcept
    {
        for (const std::uint64_t word : words_)
        {
            if (word != 0)
            {
                return false;
            }
        }
        return true;
    }

    // Intersection (domains present in both).
    [[nodiscard]] constexpr DomainBitmap operator&(const DomainBitmap& other) const noexcept
    {
        DomainBitmap result;
        for (std::uint32_t i = 0; i < DomainWordCount; ++i)
        {
            result.words_[i] = words_[i] & other.words_[i];
        }
        return result;
    }

    // In-place union (add every domain present in @other).
    constexpr DomainBitmap& operator|=(const DomainBitmap& other) noexcept
    {
        for (std::uint32_t i = 0; i < DomainWordCount; ++i)
        {
            words_[i] |= other.words_[i];
        }
        return *this;
    }

    // Remove and return the lowest domain id in the set. Precondition: !empty().
    [[nodiscard]] DomainId popLowest() noexcept
    {
        for (std::uint32_t i = 0; i < DomainWordCount; ++i)
        {
            if (words_[i] != 0)
            {
                const auto bitIndex = static_cast<DomainId>(__builtin_ctzll(words_[i]));
                words_[i] &= words_[i] - 1;
                return i * DomainBitsPerWord + bitIndex;
            }
        }
        return 0;  // unreachable given the precondition
    }

    // Iterate set domain ids low-to-high (non-destructive); enables range-for.
    class iterator
    {
    public:
        constexpr iterator(const DomainBitmap* bitmap, DomainId pos) noexcept
            : bitmap_{bitmap},
              pos_{nextSet(pos)}
        {
        }
        [[nodiscard]] constexpr DomainId operator*() const noexcept
        {
            return pos_;
        }
        constexpr iterator& operator++() noexcept
        {
            pos_ = nextSet(static_cast<DomainId>(pos_ + 1));
            return *this;
        }
        [[nodiscard]] constexpr bool operator!=(iterator other) const noexcept
        {
            return pos_ != other.pos_;
        }

    private:
        [[nodiscard]] constexpr DomainId nextSet(DomainId from) const noexcept
        {
            while (from < MaxDomains && !bitmap_->has(from))
            {
                ++from;
            }
            return from;
        }
        const DomainBitmap* bitmap_;
        DomainId pos_;
    };

    [[nodiscard]] constexpr iterator begin() const noexcept
    {
        return iterator{this, DomainId{0}};
    }
    [[nodiscard]] constexpr iterator end() const noexcept
    {
        return iterator{this, static_cast<DomainId>(MaxDomains)};
    }

private:
    static constexpr std::uint64_t bitMask(DomainId domain) noexcept
    {
        return std::uint64_t{1} << (domain % DomainBitsPerWord);
    }
    std::uint64_t words_[DomainWordCount]{};
};

}  // namespace cme
