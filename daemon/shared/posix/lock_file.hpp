// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/lock_file.hpp -- a file whose flock says one process is running under a name.
//
// The kernel arbitrates: LOCK_NB fails for whichever process loses the race, closing the window a
// look-then-start check would leave open between the check and the start.
//
// The kernel releases the lock when the descriptor closes, so a killed process frees the name. The
// file stays empty, because the kernel is the only record of who holds it that cannot go stale.

#pragma once

#include <fcntl.h>
#include <sys/file.h>

#include <cerrno>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "shared/posix/unique_fd.hpp"

namespace posix
{

class LockFile
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    LockFile(const LockFile&) = delete;
    LockFile(LockFile&&) noexcept = default;

    // Closing releases the lock. The file stays, because removing it would race a starter that has
    // already opened this same path and would leave that starter locking a name nobody can see.
    ~LockFile() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────────
    LockFile& operator=(const LockFile&) = delete;
    LockFile& operator=(LockFile&&) noexcept = default;

    // ── factories ──────────────────────────────────────────────────────
    // The lock on @path, or nothing when another process holds it. Throws std::system_error when the
    // path cannot be opened at all, which is a deployment fault rather than a busy name.
    [[nodiscard]] static std::optional<LockFile> take(const std::string& path)
    {
        UniqueFd held{::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, CreateMode)};
        if (!held)
        {
            throw std::system_error{errno, std::system_category(), "open(" + path + ")"};
        }

        if (::flock(held.get(), LOCK_EX | LOCK_NB) != 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
            {
                return std::nullopt;
            }
            throw std::system_error{errno, std::system_category(), "flock(" + path + ")"};
        }

        return LockFile{std::move(held)};
    }

    // ── accessors ──────────────────────────────────────────────────────
    [[nodiscard]] FileDesc descriptor() const noexcept
    {
        return held_.get();
    }

private:
    // Only what creating the path needs. Not the caller's to pick: the file holds nothing, and who may
    // reach the daemon is the socket's mode and the admit policy.
    static constexpr ::mode_t CreateMode = 0644;

    explicit LockFile(UniqueFd held) noexcept
        : held_{std::move(held)}
    {
    }

private:
    UniqueFd held_;
};

}  // namespace posix
