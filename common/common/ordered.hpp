// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// ordered.hpp -- a process-local atomic whose memory order its type fixes.
//
// The order belongs to the protocol a word takes part in, so it is chosen once where the word is
// declared instead of at every load and store. A word whose accesses fit neither type stays a plain
// std::atomic with its orders written out, and so does a word in memory another process maps.

#pragma once

#include <atomic>

namespace ordered
{

// A word nothing is handed over through: a flag, a count, a value read on its own.
template <typename T>
class Relaxed
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    constexpr explicit Relaxed(T initial) noexcept
        : value_{initial}
    {
    }

    // ── public methods ─────────────────────────────────────────────────
    [[nodiscard]] T load() const noexcept
    {
        return value_.load(std::memory_order_relaxed);
    }

    void store(T value) noexcept
    {
        value_.store(value, std::memory_order_relaxed);
    }

private:
    std::atomic<T> value_;
};

// A word that hands over what was written before its store to whoever loads it. reset() is for the
// start of a round, when there is nothing to hand over yet.
template <typename T>
class Published
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    constexpr explicit Published(T initial) noexcept
        : value_{initial}
    {
    }

    // ── public methods ─────────────────────────────────────────────────
    [[nodiscard]] T load() const noexcept
    {
        return value_.load(std::memory_order_acquire);
    }

    void store(T value) noexcept
    {
        value_.store(value, std::memory_order_release);
    }

    void reset(T value) noexcept
    {
        value_.store(value, std::memory_order_relaxed);
    }

private:
    std::atomic<T> value_;
};

}  // namespace ordered
