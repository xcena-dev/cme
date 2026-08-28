// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/startup/config.cpp -- see daemon/startup/config.hpp.

#include "daemon/startup/config.hpp"

// S_IWOTH's definition chain ends in bits/, which include-cleaner is told to ignore, so it credits
// neither this header for providing it nor this file for using it.
#include <sys/stat.h>  // NOLINT(misc-include-cleaner)
#include <sys/types.h>

#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "cmed/errors.hpp"
#include "common/kv_config.hpp"
#include "common/timing.hpp"
#include "shared/posix/account.hpp"

namespace cmed::daemon
{

namespace
{

// The key travels with each entry, since a refusal names the key the reader has to fix rather than
// the one account inside it that could not be resolved.
template <typename T_Id, typename T_Resolve>
[[nodiscard]] std::vector<T_Id>
readIds(const kvconfig::KeyValueConfig& values, const std::string& key, T_Resolve resolve)
{
    std::vector<T_Id> ids;
    for (const std::string& written : values.getList(key))
    {
        ids.push_back(resolve(written, key));
    }
    return ids;
}

// A zero timeout is a real "do not wait" answer, but a zero poll gap is a spin, so only the poll
// value is refused.
void refuseUnusableConfig(const DaemonConfig_t& config, const std::string& origin)
{
    if (config.area.name.empty())
    {
        throw CmedInvalidArgumentError{origin + ": area.name is empty"};
    }
    if (config.region.uri.empty())
    {
        throw CmedInvalidArgumentError{origin + ": region.uri is empty"};
    }
    if (config.socket.dir.empty())
    {
        throw CmedInvalidArgumentError{origin + ": socket.dir is empty"};
    }
    // No count stands for "decide for me". What a pool of workers bounds is how many region acquires
    // can be outstanding at once, which is the deployment's call and not this daemon's.
    if (config.workers.count == 0)
    {
        throw CmedInvalidArgumentError{origin + ": workers.count is 0, so no domain would be served"};
    }
    // Zero admits nobody, which is a daemon nothing can reach rather than a daemon with no bound.
    if (config.control.maxConnections == 0)
    {
        throw CmedInvalidArgumentError{origin + ": control.max_connections is 0, so nobody could connect"};
    }
    // A daemon that turns this slowly is not serving anyone, and the bound also keeps the staleness
    // window the area carries inside the uint32 field that holds it.
    constexpr timing::Millis LongestIdleInterval{60000};

    // Zero spins, and past the bound the wait for a dead daemon is longer than any caller's own
    // deadline. Both loops answer to it, so both are named where they fail.
    for (const auto& [key, interval] : {std::pair{"serve.idle_interval_ms", config.serve.idleInterval},
                                        std::pair{"control.idle_interval_ms", config.control.idleInterval}})
    {
        if (interval == timing::Millis::zero())
        {
            throw CmedInvalidArgumentError{origin + ": " + key + " is 0, which spins"};
        }
        if (interval > LongestIdleInterval)
        {
            throw CmedInvalidArgumentError{origin + ": " + key + " is over " +
                                           std::to_string(LongestIdleInterval.count()) + "ms"};
        }
    }

    // A run must end while the grants it issued are still worth acting on, or the turn would
    // still be held here after a requester was told to stop trusting it.
    if (config.cohort.hold >= config.cohort.grantValidity)
    {
        throw CmedInvalidArgumentError{origin +
                                       ": cohort.hold_ms is not under cohort.grant_validity_ms"};
    }

    // A requester that connects receives a mapping it can write any word of, so the socket's mode
    // is the whole admission boundary the filesystem draws.
    if ((config.socket.mode & S_IWOTH) != 0)  // NOLINT(misc-include-cleaner)
    {
        throw CmedInvalidArgumentError{origin + ": socket.mode is world-writable"};
    }
}

}  // namespace

DaemonConfig_t loadDaemonConfig(const std::string& path)
{
    const kvconfig::KeyValueConfig values = kvconfig::KeyValueConfig::loadIfPresent(path);

    DaemonConfig_t config;
    config.area.name = values.getString("area.name", config.area.name);
    config.socket.dir = values.getString("socket.dir", config.socket.dir);
    config.socket.mode = static_cast<mode_t>(values.getMode("socket.mode", config.socket.mode));
    config.admit.uids = readIds<::uid_t>(values, "admit.uids", posix::getUid);
    config.admit.gids = readIds<::gid_t>(values, "admit.gids", posix::getGid);
    config.region.uri = values.getString("region.uri", config.region.uri);
    config.region.coherency = values.getString("region.coherency", config.region.coherency);
    config.region.formatTimeout = values.get("region.format_timeout_ms", config.region.formatTimeout);
    config.cohort.hold = values.get("cohort.hold_ms", config.cohort.hold);
    config.cohort.grantValidity = values.get("cohort.grant_validity_ms", config.cohort.grantValidity);
    config.workers.count = values.get("workers.count", config.workers.count);
    config.serve.idleInterval = values.get("serve.idle_interval_ms", config.serve.idleInterval);
    config.control.idleInterval = values.get("control.idle_interval_ms", config.control.idleInterval);
    config.control.maxConnections = values.get("control.max_connections", config.control.maxConnections);
    config.workers.spin = values.get("workers.spin_us", config.workers.spin);
    config.maintenance.interval = values.get("maintenance.interval_ms", config.maintenance.interval);
    config.serve.spin = values.get("serve.spin_us", config.serve.spin);
    config.registry.refreshInterval = values.get("registry.refresh_interval_ms", config.registry.refreshInterval);
    config.registry.deleteWaitTimeout = values.get("registry.delete_wait_timeout_ms", config.registry.deleteWaitTimeout);

    refuseUnusableConfig(config, values.origin());
    return config;
}

}  // namespace cmed::daemon
