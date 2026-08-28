// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cmed/robust_lock.hpp -- holding one PTHREAD_MUTEX_ROBUST mutex out of the shared area.
//
// The span from LockRequested to Complete can throw, so release belongs to a destructor rather
// than a call at the end of the happy path.
//
// RECOVERY: an EOWNERDEAD mutex must be made consistent before unlock, or it goes NOTRECOVERABLE for the life of the area.

#pragma once

#include <pthread.h>  // IWYU pragma: keep (declares pthread_mutex_t through bits/pthreadtypes.h)

#include <optional>

namespace cmed::util
{

// Move-only, because two owners would unlock one mutex twice and the second unlock would release a
// span the first owner's successor is inside.
class RobustLock
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // Holds nothing, for a caller that locks conditionally and moves the result in later.
    RobustLock() noexcept = default;

    // Blocks until the mutex is this thread's. Throws CmedBackendError when the failure is neither a
    // grant nor an abandoned holder.
    explicit RobustLock(pthread_mutex_t& mutex);

    RobustLock(const RobustLock&) = delete;
    RobustLock(RobustLock&& other) noexcept;
    ~RobustLock() noexcept;

    // ── operator= ──────────────────────────────────────────────────
    RobustLock& operator=(const RobustLock&) = delete;
    RobustLock& operator=(RobustLock&& other) noexcept;

    // ── factories ──────────────────────────────────────────────────
    // nullopt when another live holder has it. That is the daemon's liveness test: a slot whose
    // mutex answers EBUSY belongs to a process that is still running.
    [[nodiscard]] static std::optional<RobustLock> tryLock(pthread_mutex_t& mutex);

    // ── public methods ─────────────────────────────────────────────
    // Explicit early release. Idempotent.
    void unlock() noexcept;

    // ── accessors ──────────────────────────────────────────────────
    // The previous holder died inside its critical section, so whatever it guarded may be half
    // written. The mutex itself is already consistent by the time a caller can ask.
    [[nodiscard]] bool wasAbandoned() const noexcept
    {
        return abandoned_;
    }

    explicit operator bool() const noexcept
    {
        return mutex_ != nullptr;
    }

private:
    RobustLock(pthread_mutex_t* mutex, bool abandoned) noexcept;

private:
    pthread_mutex_t* mutex_{nullptr};
    bool abandoned_{false};
};

}  // namespace cmed::util
