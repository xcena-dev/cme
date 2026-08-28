// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.

#include "daemon/startup/served_area.hpp"

#include <pthread.h>

#include <cstdint>
#include <new>  // NOLINT(misc-include-cleaner): placement new's operator is declared here
#include <string>
#include <string_view>
#include <utility>

#include "cme/shared.hpp"
#include "cmed/errors.hpp"
#include "daemon/startup/config.hpp"
#include "shared/area.hpp"
#include "shared/posix/mem_file.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::daemon
{

namespace
{

// ── the area's mutexes ─────────────────────────────────────────────

// PROCESS_SHARED because the holder and the next locker are different processes, ROBUST because a
// holder that dies must hand the next locker EOWNERDEAD rather than wedge the domain.
void initSharedMutex(pthread_mutex_t& mutex)
{
    pthread_mutexattr_t attributes;
    std::int32_t result = ::pthread_mutexattr_init(&attributes);
    if (result != 0)
    {
        throw CmedBackendError{"cmed::daemon pthread_mutexattr_init", threadError(result)};
    }

    result = ::pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);
    if (result == 0)
    {
        result = ::pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST);
    }
    if (result == 0)
    {
        result = ::pthread_mutex_init(&mutex, &attributes);
    }

    ::pthread_mutexattr_destroy(&attributes);
    if (result != 0)
    {
        throw CmedBackendError{"cmed::daemon robust mutex init", threadError(result)};
    }
}

void initMutexes(protocol::SharedArea_t& area)
{
    initSharedMutex(area.daemon.liveness);

    for (protocol::Domain_t& domain : area.domain.table)
    {
        initSharedMutex(domain.request.lock);
    }
}

// ── the region ─────────────────────────────────────────────────────

// The name config carries, as the mode libcme takes. Throws rather than falling back on a name this
// does not know: the wrong mode is silent on a coherent host and wrong everywhere else.
[[nodiscard]] cme::CoherencyMode getCoherencyMode(const std::string& name)
{
    if (name == "cache_coherent")
    {
        return cme::CoherencyMode::CacheCoherent;
    }
    if (name == "uncached")
    {
        return cme::CoherencyMode::Uncached;
    }
    if (name == "flush")
    {
        return cme::CoherencyMode::Flush;
    }

    throw CmedInvalidArgumentError{"cmed: region.coherency is not a mode: " + name};
}

// Open, never format: formatting zeroes the region, and the peers on other nodes are using it.
// Throws whatever libcme throws; a region that will not open is not something to serve around.
[[nodiscard]] cme::Session openCmeSession(const DaemonConfig_t& config)
{
    cme::Session::OpenOpts_t opts;
    opts.coherency = getCoherencyMode(config.region.coherency);
    opts.formatTimeout = config.region.formatTimeout;

    return cme::Session::open(config.region.uri, opts);
}

}  // namespace

// ── the area ───────────────────────────────────────────────────────

CmedArea formatArea(std::string_view label)
{
    posix::MemFile file = posix::MemFile::create(std::string{label}, sizeof(protocol::SharedArea_t));

    // A fresh memfd already reads as zero. The placement new is what starts the object's lifetime
    // there, which the atomics and the mutexes below need before anything touches them.
    auto* area = new (file.base()) protocol::SharedArea_t{};
    initMutexes(*area);

    area->publishReady(static_cast<std::uint32_t>(file.bytes()));

    return CmedArea::adopt(std::move(file), area);
}

// ── becoming the daemon ────────────────────────────────────────────

ServedArea::ServedArea(const DaemonConfig_t& config)
    : session_{openCmeSession(config)},
      area_{formatArea(config.area.name)},
      // A requester that can take this mutex has found a daemon that is not there, which is a
      // different answer from one that is merely slow.
      alive_{area_.shared().daemon.liveness}
{
}

}  // namespace cmed::daemon
