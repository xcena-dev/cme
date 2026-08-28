// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/unique_fd.hpp -- one file descriptor, owned.
//
// Not a unique_ptr bent into the role: the empty state is -1, not null, so every use would read as
// an indirection that is not there.
//
// Both trees acquire descriptors on paths that can throw between the acquire and the use, so a close
// written at each exit is a close missed at one of them. Moving the owner hands ownership on instead.

#pragma once

#include <unistd.h>

#include <cstdint>
#include <utility>

namespace posix
{

// A descriptor number, as the kernel hands it back. A plain alias rather than a distinct type: it
// reads as a descriptor at a glance, and mixing one with a count stays the caller's to notice.
using FileDesc = std::int32_t;

class UniqueFd
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // Holds nothing. -1 is that same state, so a failed syscall's answer can be adopted directly and
    // asked about with operator bool.
    UniqueFd() noexcept = default;

    explicit UniqueFd(FileDesc taken) noexcept
        : held_{taken}
    {
    }

    UniqueFd(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept
        : held_{other.release()}
    {
    }

    ~UniqueFd() noexcept
    {
        reset();
    }

    // ── operator= ──────────────────────────────────────────────────
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            held_ = other.release();
        }
        return *this;
    }

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] FileDesc get() const noexcept
    {
        return held_;
    }

    explicit operator bool() const noexcept
    {
        return held_ >= 0;
    }

private:
    // Both serve the move and the destructor and nothing else: a caller hands a descriptor on by moving
    // the owner, and closes early by letting it go out of scope.
    [[nodiscard]] FileDesc release() noexcept
    {
        return std::exchange(held_, -1);
    }

    // The result goes unread: a failing close on a descriptor nobody will use again leaves a caller
    // nothing to do about it.
    void reset() noexcept
    {
        if (held_ >= 0)
        {
            static_cast<void>(::close(std::exchange(held_, -1)));
        }
    }

    FileDesc held_{-1};
};

}  // namespace posix
