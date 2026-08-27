// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// common/bitmap.hpp -- a fixed set of ids packed into uint64 words, in two forms.
//
// Bits is a value: copied, intersected, drained by popping the lowest id. Whoever owns the words
// loads them into one, operates, and stores them back, so the concurrency lives above it.
//
// AtomicBits is the same words held for several threads at once. Its operations answer what a bit
// was rather than what it became, since a claim is the previous value; takeSnapshot() hands the value
// form to whatever wants to iterate.

#pragma once

#include <atomic>
#include <cstdint>

namespace bitmap
{

// The element's bit width, not a tunable: a caller names an id and never a word.
inline constexpr std::uint32_t BitsPerWord = 64;

// No id to be had. Distinct from an id, which is why the largest value stands for it.
inline constexpr std::uint32_t NoIndex = ~std::uint32_t{0};

[[nodiscard]] constexpr std::uint32_t countWords(std::uint32_t bitCount) noexcept
{
    return (bitCount + BitsPerWord - 1) / BitsPerWord;
}

[[nodiscard]] constexpr std::uint64_t makeMask(std::uint32_t index) noexcept
{
    return std::uint64_t{1} << (index % BitsPerWord);
}

// ── the value form ─────────────────────────────────────────────────

template <std::uint32_t bitCount>
class Bits
{
public:
    static constexpr std::uint32_t WordCount = countWords(bitCount);

    constexpr Bits() noexcept = default;

    // ── raw words, for a load or store against whatever holds them ─
    constexpr void setWord(std::uint32_t index, std::uint64_t value) noexcept
    {
        words_[index] = value;
    }
    [[nodiscard]] constexpr std::uint64_t getWord(std::uint32_t index) const noexcept
    {
        return words_[index];
    }

    // Contiguous, for a bulk copy: host-endian, low word holds the low ids. Spans WordCount words.
    [[nodiscard]] constexpr std::uint64_t* getData() noexcept
    {
        return words_;
    }
    [[nodiscard]] constexpr const std::uint64_t* getData() const noexcept
    {
        return words_;
    }

    // ── one id at a time ───────────────────────────────────────────
    constexpr void set(std::uint32_t index) noexcept
    {
        words_[index / BitsPerWord] |= makeMask(index);
    }
    constexpr void clear(std::uint32_t index) noexcept
    {
        words_[index / BitsPerWord] &= ~makeMask(index);
    }
    [[nodiscard]] constexpr bool has(std::uint32_t index) const noexcept
    {
        return (words_[index / BitsPerWord] & makeMask(index)) != 0;
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

    // ── whole sets ─────────────────────────────────────────────────
    // Intersection: the ids present in both.
    [[nodiscard]] constexpr Bits operator&(const Bits& other) const noexcept
    {
        Bits result;
        for (std::uint32_t word = 0; word < WordCount; ++word)
        {
            result.words_[word] = words_[word] & other.words_[word];
        }
        return result;
    }

    // In-place union: add every id present in @other.
    constexpr Bits& operator|=(const Bits& other) noexcept
    {
        for (std::uint32_t word = 0; word < WordCount; ++word)
        {
            words_[word] |= other.words_[word];
        }
        return *this;
    }

    // In-place difference: drop every id present in @other. What a caller holding a mask of ids it
    // may not touch asks for, since the negation of a set would set the padding bits above bitCount.
    constexpr Bits& dropAll(const Bits& other) noexcept
    {
        for (std::uint32_t word = 0; word < WordCount; ++word)
        {
            words_[word] &= ~other.words_[word];
        }
        return *this;
    }

    // Removes and answers the lowest id in the set. Precondition: !isEmpty().
    [[nodiscard]] std::uint32_t popLowest() noexcept
    {
        for (std::uint32_t word = 0; word < WordCount; ++word)
        {
            if (words_[word] != 0)
            {
                const auto offset = static_cast<std::uint32_t>(__builtin_ctzll(words_[word]));
                words_[word] &= words_[word] - 1;
                return (word * BitsPerWord) + offset;
            }
        }
        return 0;  // unreachable given the precondition
    }

    // ── iterating what is set ──────────────────────────────────────
    // Low to high and non-destructive, which is what makes a range-for over this work.
    class iterator
    {
    public:
        constexpr iterator(const Bits* holding, std::uint32_t from) noexcept
            : holding_{holding},
              index_{nextSet(from)}
        {
        }
        [[nodiscard]] constexpr std::uint32_t operator*() const noexcept
        {
            return index_;
        }
        constexpr iterator& operator++() noexcept
        {
            index_ = nextSet(index_ + 1);
            return *this;
        }
        [[nodiscard]] constexpr bool operator!=(iterator other) const noexcept
        {
            return index_ != other.index_;
        }

    private:
        [[nodiscard]] constexpr std::uint32_t nextSet(std::uint32_t from) const noexcept
        {
            while (from < bitCount && !holding_->has(from))
            {
                ++from;
            }
            return from;
        }
        const Bits* holding_;
        std::uint32_t index_;
    };

    [[nodiscard]] constexpr iterator begin() const noexcept
    {
        return iterator{this, 0};
    }
    [[nodiscard]] constexpr iterator end() const noexcept
    {
        return iterator{this, bitCount};
    }

private:
    std::uint64_t words_[WordCount]{};
};

// ── the concurrent form ────────────────────────────────────────────

template <std::uint32_t bitCount>
class AtomicBits
{
public:
    static constexpr std::uint32_t WordCount = countWords(bitCount);

    // Whether this call was the one that raised @index. False means it already stood, which is what a
    // caller claiming exclusive use of an id reads as a refusal. Idempotent for a caller that ignores it.
    [[nodiscard]] bool claim(std::uint32_t index) noexcept
    {
        const std::uint64_t before =
            words_[index / BitsPerWord].fetch_or(makeMask(index), std::memory_order_acq_rel);
        return (before & makeMask(index)) == 0;
    }

    // Whether @index stood before this call took it down.
    [[nodiscard]] bool release(std::uint32_t index) noexcept
    {
        const std::uint64_t before =
            words_[index / BitsPerWord].fetch_and(~makeMask(index), std::memory_order_acq_rel);
        return (before & makeMask(index)) != 0;
    }

    [[nodiscard]] bool has(std::uint32_t index) const noexcept
    {
        return (words_[index / BitsPerWord].load(std::memory_order_acquire) & makeMask(index)) != 0;
    }

    // The lowest id that stood, taken down as it is answered, or NoIndex when none did. Retried,
    // because a taker that lost the bit to another one has to look again rather than answer an id
    // it does not hold.
    [[nodiscard]] std::uint32_t takeLowest() noexcept
    {
        for (std::uint32_t word = 0; word < WordCount; ++word)
        {
            while (true)
            {
                const std::uint64_t standing = words_[word].load(std::memory_order_acquire);
                if (standing == 0)
                {
                    break;
                }

                const auto offset = static_cast<std::uint32_t>(__builtin_ctzll(standing));
                const std::uint64_t taking = std::uint64_t{1} << offset;
                if ((words_[word].fetch_and(~taking, std::memory_order_acq_rel) & taking) != 0)
                {
                    return (word * BitsPerWord) + offset;
                }
            }
        }
        return NoIndex;
    }

    // Every word as it stands, for a caller that iterates. One word at a time, so a set spanning more
    // than one is not one instant of the whole.
    [[nodiscard]] Bits<bitCount> takeSnapshot() const noexcept
    {
        Bits<bitCount> taken;
        for (std::uint32_t word = 0; word < WordCount; ++word)
        {
            taken.setWord(word, words_[word].load(std::memory_order_acquire));
        }
        return taken;
    }

    // Every id that stood, taken down as it is answered. What one pass of a drain does, for a caller
    // that will serve all of them rather than deciding which to leave.
    [[nodiscard]] Bits<bitCount> takeAll() noexcept
    {
        Bits<bitCount> taken;
        for (std::uint32_t word = 0; word < WordCount; ++word)
        {
            taken.setWord(word, words_[word].exchange(0, std::memory_order_acq_rel));
        }
        return taken;
    }

    // Takes down exactly the ids @taking holds. What a caller that took a snapshot, decided, and is now
    // publishing that decision needs: an id raised since the takeSnapshot is not in @taking and stands.
    void dropAll(const Bits<bitCount>& taking) noexcept
    {
        for (std::uint32_t word = 0; word < WordCount; ++word)
        {
            words_[word].fetch_and(~taking.getWord(word), std::memory_order_acq_rel);
        }
    }

private:
    std::atomic<std::uint64_t> words_[WordCount]{};
};

}  // namespace bitmap
