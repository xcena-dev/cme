// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/util/robust_lock.cpp -- see cmed/robust_lock.hpp.

#include "cmed/robust_lock.hpp"

#include <pthread.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <utility>

#include "cmed/errors.hpp"

namespace cmed::util
{

namespace
{

// Called with the mutex held and EOWNERDEAD seen. Failing here still unlocks it inconsistent, which
// turns it NOTRECOVERABLE for good, so this throws rather than hand back a lock guarding nothing.
void makeConsistent(pthread_mutex_t& mutex)
{
    const std::int32_t result = ::pthread_mutex_consistent(&mutex);
    if (result != 0)
    {
        ::pthread_mutex_unlock(&mutex);
        throw CmedBackendError{"cmed::RobustLock pthread_mutex_consistent", threadError(result)};
    }
}

}  // namespace

RobustLock::RobustLock(pthread_mutex_t* mutex, bool abandoned) noexcept
    : mutex_{mutex},
      abandoned_{abandoned}
{
}

RobustLock::RobustLock(pthread_mutex_t& mutex)
{
    const std::int32_t result = ::pthread_mutex_lock(&mutex);
    if (result != 0 && result != EOWNERDEAD)
    {
        throw CmedBackendError{"cmed::RobustLock pthread_mutex_lock", threadError(result)};
    }

    if (result == EOWNERDEAD)
    {
        makeConsistent(mutex);
        abandoned_ = true;
    }

    // Last, so a throw out of makeConsistent leaves this object owning nothing and the destructor
    // does not unlock a mutex the throw already released.
    mutex_ = &mutex;
}

RobustLock::RobustLock(RobustLock&& other) noexcept
    : mutex_{std::exchange(other.mutex_, nullptr)},
      abandoned_{std::exchange(other.abandoned_, false)}
{
}

RobustLock& RobustLock::operator=(RobustLock&& other) noexcept
{
    if (this != &other)
    {
        unlock();
        mutex_ = std::exchange(other.mutex_, nullptr);
        abandoned_ = std::exchange(other.abandoned_, false);
    }
    return *this;
}

RobustLock::~RobustLock() noexcept
{
    unlock();
}

std::optional<RobustLock> RobustLock::tryLock(pthread_mutex_t& mutex)
{
    const std::int32_t result = ::pthread_mutex_trylock(&mutex);
    if (result == EBUSY)
    {
        return std::nullopt;
    }
    if (result != 0 && result != EOWNERDEAD)
    {
        throw CmedBackendError{"cmed::RobustLock pthread_mutex_trylock", threadError(result)};
    }

    if (result == EOWNERDEAD)
    {
        makeConsistent(mutex);
    }

    return RobustLock{&mutex, result == EOWNERDEAD};
}

void RobustLock::unlock() noexcept
{
    if (mutex_ != nullptr)
    {
        ::pthread_mutex_unlock(mutex_);
        mutex_ = nullptr;
        abandoned_ = false;
    }
}

}  // namespace cmed::util
