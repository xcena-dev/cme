// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/event_fd.hpp -- a counter a writer can raise and a poller can wait on.
//
// A wait parked in epoll can include it, unlike a bare flag: epoll cannot see a bool change, and a
// loop that polled one would not be parked.
//
// post() is signal-handler safe: one write of a fixed buffer, no allocation, nothing that can throw.
// That is why a stop arrives this way instead of as a flag the handler sets.

#pragma once

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <system_error>
#include <utility>

#include "shared/posix/unique_fd.hpp"

namespace posix
{

class EventFd
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    EventFd() noexcept = default;

    EventFd(const EventFd&) = delete;
    EventFd(EventFd&&) noexcept = default;
    ~EventFd() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────
    EventFd& operator=(const EventFd&) = delete;
    EventFd& operator=(EventFd&&) noexcept = default;

    // ── factories ──────────────────────────────────────────────────
    // Non-blocking, so drain() on an already-empty counter answers rather than parks.
    [[nodiscard]] static EventFd create()
    {
        UniqueFd held{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
        if (!held)
        {
            throw std::system_error{errno, std::system_category(), "eventfd"};
        }
        return EventFd{std::move(held)};
    }

    // ── public methods ─────────────────────────────────────────────
    // Adds one to the counter, waking whoever is polling. Safe from a signal handler. The result is
    // reported rather than thrown, because a handler has nowhere to put an exception.
    [[nodiscard]] bool post() const noexcept
    {
        const std::uint64_t one = 1;
        return ::write(held_.get(), &one, sizeof(one)) == static_cast<::ssize_t>(sizeof(one));
    }

    // Takes the counter back to zero. False when it was already there.
    [[nodiscard]] bool drain() const noexcept
    {
        std::uint64_t sank = 0;
        return ::read(held_.get(), &sank, sizeof(sank)) == static_cast<::ssize_t>(sizeof(sank));
    }

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] FileDesc descriptor() const noexcept
    {
        return held_.get();
    }

    explicit operator bool() const noexcept
    {
        return static_cast<bool>(held_);
    }

private:
    explicit EventFd(UniqueFd held) noexcept
        : held_{std::move(held)}
    {
    }

private:
    UniqueFd held_;
};

}  // namespace posix
