// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/mapping.hpp -- one shared mapping over a descriptor.
//
// A descriptor, not a name: the daemon creates the backing file as a memfd and the requester
// receives it already made, so no name exists that could be squatted or adopted by mistake.
//
// Knows nothing about what the bytes mean; area.hpp puts the cmed layout on top so the sizing and
// version rules stay in one place and testable on their own.

#pragma once

#include <sys/mman.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>

#include "shared/posix/unique_fd.hpp"

namespace posix
{

// Owns one mapping. Move-only, because two owners would unmap one address twice.
class SharedMapping
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    SharedMapping() noexcept = default;

    SharedMapping(const SharedMapping&) = delete;

    SharedMapping(SharedMapping&& other) noexcept
        : base_{other.base_},
          bytes_{other.bytes_}
    {
        other.base_ = nullptr;
        other.bytes_ = 0;
    }

    ~SharedMapping() noexcept
    {
        unmap();
    }

    // ── operator= ──────────────────────────────────────────────────────
    SharedMapping& operator=(const SharedMapping&) = delete;

    SharedMapping& operator=(SharedMapping&& other) noexcept
    {
        if (this != &other)
        {
            unmap();
            base_ = other.base_;
            bytes_ = other.bytes_;
            other.base_ = nullptr;
            other.bytes_ = 0;
        }
        return *this;
    }

    // ── factories ──────────────────────────────────────────────────────
    // MAP_SHARED, so every process mapping this descriptor sees one set of words. The mapping outlives
    // the descriptor, which is what lets the caller close its own copy and keep the memory.
    [[nodiscard]] static SharedMapping mmap(FileDesc descriptor, std::uint64_t bytes)
    {
        void* const mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
        if (mapped == MAP_FAILED)
        {
            throw std::system_error{errno, std::system_category(),
                                    "mmap(" + std::to_string(bytes) + " bytes)"};
        }
        return SharedMapping{mapped, bytes};
    }

    // ── accessors ──────────────────────────────────────────────────────
    [[nodiscard]] void* base() const noexcept
    {
        return base_;
    }

    [[nodiscard]] std::uint64_t bytes() const noexcept
    {
        return bytes_;
    }

    explicit operator bool() const noexcept
    {
        return base_ != nullptr;
    }

private:
    SharedMapping(void* base, std::uint64_t bytes) noexcept
        : base_{base},
          bytes_{bytes}
    {
    }

    void unmap() noexcept
    {
        if (base_ != nullptr)
        {
            ::munmap(base_, bytes_);
            base_ = nullptr;
            bytes_ = 0;
        }
    }

private:
    void* base_{nullptr};
    std::uint64_t bytes_{0};
};

}  // namespace posix
