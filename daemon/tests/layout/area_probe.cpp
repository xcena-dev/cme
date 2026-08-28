// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// area_probe.cpp -- the shared area comes up, and its mutexes are what the design says.
//
// The mutex case forks. PROCESS_SHARED and ROBUST are the two attributes the whole recovery story
// rests on, and neither shows itself inside one process: a same-process lock succeeds whether or
// not the attributes were set. A child that dies holding the lock is what makes the parent's
// EOWNERDEAD proof rather than intention.

#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <system_error>
#include <utility>

#include "cmed/errors.hpp"
#include "common/bitmap.hpp"
#include "daemon/startup/served_area.hpp"
#include "harness/helper_area.hpp"
#include "shared/area.hpp"
#include "shared/posix/unique_fd.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

// Reaches /proc and nothing else. The area has no name to be found by.
constexpr const char* AreaName = "probe";

bool publishesItsHeader(const cmed::CmedArea& area)
{
    const cmed::protocol::SharedArea_t& shared = area.shared();
    return shared.getAbiVersion() == cmed::protocol::AbiVersion &&
           cmed::harness::getMaxDomains(shared) == cmed::MaxDomains &&
           shared.getAreaBytes() == sizeof(cmed::protocol::SharedArea_t);
}

// Two mappings of one object, so a word written through the first must be readable through the
// second. That is the whole point of the area and nothing else in this file checks it.
bool sharesWritesBetweenMappings(cmed::CmedArea& creator, cmed::CmedArea& joiner)
{
    const std::uint32_t domainId = 7;
    if (!cmed::harness::setPendingBit(creator.shared(), domainId))
    {
        return false;
    }

    const std::uint64_t expected = bitmap::makeMask(domainId);
    const std::uint32_t word = domainId / bitmap::BitsPerWord;
    const bool seen = cmed::harness::getPendingWord(joiner.shared(), word) == expected;

    return seen && cmed::harness::clearPendingBit(creator.shared(), domainId);
}

// The child takes the lock and dies still holding it. A parent that gets EOWNERDEAD saw across a
// process boundary and saw the death, which is both attributes at once.
bool handsBackAnOwnerDeadLock(cmed::CmedArea& area)
{
    pthread_mutex_t& lock = cmed::harness::resolveDomainMutex(area.shared(), 3);

    const auto child = ::fork();
    if (child < 0)
    {
        return false;
    }
    if (child == 0)
    {
        // _exit, not return: an unwound stack would run the parent's atexit handlers in this copy.
        ::_exit(::pthread_mutex_lock(&lock) == 0 ? 0 : 1);
    }

    int status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        return false;
    }

    // trylock, not lock: it is what the daemon uses to judge a dead requester, and a mutex that
    // was left STALLED answers EBUSY here instead of blocking this probe forever.
    if (::pthread_mutex_trylock(&lock) != EOWNERDEAD)
    {
        return false;
    }

    return ::pthread_mutex_consistent(&lock) == 0 && ::pthread_mutex_unlock(&lock) == 0;
}

// A second mapping of one area, taken the way a requester takes it: its own copy of the descriptor,
// mapped and version-checked on its own.
[[nodiscard]] cmed::CmedArea joinArea(const cmed::CmedArea& creator)
{
    posix::UniqueFd copied{::fcntl(creator.descriptor(), F_DUPFD_CLOEXEC, 0)};
    if (!copied)
    {
        throw cmed::CmedBackendError{"area probe cannot copy the descriptor", cmed::lastSystemError()};
    }
    return cmed::CmedArea::attach(std::move(copied));
}

// What is left of "attach to something that is not there" once names are gone. The descriptor is the
// only way in, so a descriptor that refers to nothing is the whole of that case.
bool refusesADescriptorThatIsNotThere()
{
    try
    {
        // The cast is what the call is for: this line is expected to throw, not to return an area.
        static_cast<void>(cmed::CmedArea::attach(posix::UniqueFd{-1}));
    }
    catch (const cmed::CmedBackendError& failure)
    {
        return failure.code() == std::errc::bad_file_descriptor;
    }
    catch (const cmed::CmedError&)
    {
        return false;
    }
    return false;
}

}  // namespace

int main()
{
    bool passed = false;
    try
    {
        cmed::CmedArea creator = cmed::daemon::formatArea(AreaName);
        cmed::CmedArea joiner = joinArea(creator);

        passed = publishesItsHeader(creator) &&
                 publishesItsHeader(joiner) &&
                 sharesWritesBetweenMappings(creator, joiner) &&
                 handsBackAnOwnerDeadLock(creator) &&
                 refusesADescriptorThatIsNotThere();

        std::printf("area=%zu bytes name=%s\n",
                    sizeof(cmed::protocol::SharedArea_t), AreaName);
    }
    catch (const cmed::CmedError& failure)
    {
        std::printf("area probe threw: %s\n", failure.what());
        passed = false;
    }

    return passed ? 0 : 1;
}
