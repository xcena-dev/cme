// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/epoll.hpp -- one wait that covers every descriptor a loop cares about.
//
// The descriptors go in, and the wait answers for whichever is ready, so no caller picks one
// descriptor to block on and has to interrupt it for the other.
//
// EINTR needs no branch: an interrupted wait reads as nothing ready, and the next pass sees
// whatever a handler left behind.

#pragma once

#include <sys/epoll.h>

#include <cerrno>
#include <cstdint>
#include <system_error>
#include <utility>
#include <vector>

#include "common/timing.hpp"
#include "shared/posix/unique_fd.hpp"

namespace posix
{

// Several epoll bits at once, which is what both a registration and a wake-up carry. Plural on purpose:
// one value here is a set, so it is tested with a mask and never compared for equality.
using EventMask = std::uint32_t;

// The bits a wait answers with, and the ones a caller registers. One name per bit, so a caller reads a
// mask rather than the macro set the kernel spells them with.
struct EpollFlag
{
    static constexpr EventMask Readable = EPOLLIN;
    static constexpr EventMask PeerGone = EPOLLRDHUP;
    static constexpr EventMask HungUp = EPOLLHUP;
    static constexpr EventMask Failed = EPOLLERR;
};

// Whether @reported carries any bit of @asked. A wake-up may carry several at once, so this is a mask
// test and never an equality.
[[nodiscard]] constexpr bool hasEvent(EventMask reported, EventMask asked) noexcept
{
    return (reported & asked) != 0;
}

class Epoll
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    Epoll() noexcept = default;

    Epoll(const Epoll&) = delete;
    Epoll(Epoll&&) noexcept = default;
    ~Epoll() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────
    Epoll& operator=(const Epoll&) = delete;
    Epoll& operator=(Epoll&&) noexcept = default;

    // ── nested types ───────────────────────────────────────────────
    // One ready descriptor, as the two things a caller acts on. The kernel's own epoll_event carries a
    // union a caller would have to know how this class filled.
    struct Ready_t
    {
        FileDesc descriptor;
        EventMask events;
    };

    // What one wait() answered for, walked with a range for. It borrows the buffer rather than copying
    // it, so it stays valid only until the next wait().
    class Pass
    {
    public:
        // Reads the entry at its position on dereference, so nothing is converted for an entry a
        // caller breaks before reaching.
        class Iterator
        {
        public:
            Iterator(const ::epoll_event* filled, std::uint32_t index) noexcept
                : filled_{filled},
                  index_{index}
            {
            }

            [[nodiscard]] Ready_t operator*() const noexcept
            {
                const ::epoll_event& entry = filled_[index_];
                return Ready_t{static_cast<FileDesc>(entry.data.u64), entry.events};
            }

            Iterator& operator++() noexcept
            {
                ++index_;
                return *this;
            }

            [[nodiscard]] bool operator!=(const Iterator& other) const noexcept
            {
                return index_ != other.index_;
            }

        private:
            const ::epoll_event* filled_;
            std::uint32_t index_;
        };

        Pass(const ::epoll_event* filled, std::uint32_t seen) noexcept
            : filled_{filled},
              seen_{seen}
        {
        }

        // ── accessors ──────────────────────────────────────────────
        [[nodiscard]] Iterator begin() const noexcept
        {
            return Iterator{filled_, 0};
        }

        [[nodiscard]] Iterator end() const noexcept
        {
            return Iterator{filled_, seen_};
        }

        // Zero is a normal answer, from the deadline or a signal.
        [[nodiscard]] std::uint32_t size() const noexcept
        {
            return seen_;
        }

    private:
        const ::epoll_event* filled_;
        std::uint32_t seen_;
    };

    // ── factories ──────────────────────────────────────────────────
    // @atOnce bounds one pass: past that the rest waits for the next wait(), so a burst cannot keep a
    // caller from reaching whatever else its loop does at the top.
    [[nodiscard]] static Epoll create(std::uint32_t atOnce)
    {
        UniqueFd held{::epoll_create1(EPOLL_CLOEXEC)};
        if (!held)
        {
            throw std::system_error{errno, std::system_category(), "epoll_create1"};
        }
        return Epoll{std::move(held), atOnce};
    }

    // ── public methods ─────────────────────────────────────────────
    // The descriptor is its own token, so a ready entry names itself and needs no table beside it.
    void watch(FileDesc descriptor, EventMask events)
    {
        ::epoll_event wanted = {};
        wanted.events = events;
        wanted.data.u64 = static_cast<std::uint64_t>(descriptor);
        if (::epoll_ctl(held_.get(), EPOLL_CTL_ADD, descriptor, &wanted) != 0)
        {
            throw std::system_error{errno, std::system_category(), "epoll_ctl(add)"};
        }
    }

    // What this pass answered for, walked with a range for. The result borrows this object's buffer and
    // the next call to this refills it.
    [[nodiscard]] Pass wait(timing::Millis limit)
    {
        const std::int32_t seen = ::epoll_wait(held_.get(), filled_.data(),
                                               static_cast<std::int32_t>(filled_.size()),
                                               static_cast<std::int32_t>(limit.count()));
        if (seen < 0)
        {
            if (errno == EINTR)
            {
                return Pass{filled_.data(), 0};
            }
            throw std::system_error{errno, std::system_category(), "epoll_wait"};
        }
        return Pass{filled_.data(), static_cast<std::uint32_t>(seen)};
    }

private:
    Epoll(UniqueFd held, std::uint32_t atOnce)
        : held_{std::move(held)},
          filled_(atOnce)
    {
    }

private:
    UniqueFd held_;

    // The kernel writes here on every wait(), so it is this object's rather than a caller's.
    std::vector<::epoll_event> filled_;
};

}  // namespace posix
