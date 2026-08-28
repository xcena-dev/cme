// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// client/config.cpp -- see cmed/config.hpp.

#include "cmed/config.hpp"

#include <string>

#include "cmed/errors.hpp"
#include "common/kv_config.hpp"
#include "common/timing.hpp"
#include "shared/protocol/socket_path.hpp"

namespace cmed
{

namespace
{

// Only the rendezvous. Every deadline means something at zero, where it says do not wait at all.
void refuseUnusableConfig(const CmedClientConfig_t& config, const std::string& origin, const std::string& daemonOrigin)
{
    if (config.socketPath.empty())
    {
        throw CmedInvalidArgumentError{daemonOrigin + ": socket.dir or area.name is empty"};
    }
}

}  // namespace

CmedClientConfig_t loadClientConfig(const std::string& path, const std::string& daemonPath)
{
    const auto values = kvconfig::KeyValueConfig::loadIfPresent(path);
    const auto published = kvconfig::KeyValueConfig::loadIfPresent(daemonPath);

    CmedClientConfig_t config;

    // From the daemon's file: it bound this path, so an application naming its own would name a
    // socket nobody listens on.
    const auto directory = published.getString("socket.dir", "");
    const auto areaName = published.getString("area.name", "");

    // Joined only once both halves are present; buildSocketPath on two empty strings answers `/.sock`,
    // which reads as a path rather than as the missing keys it is.
    if (!directory.empty() && !areaName.empty())
    {
        config.socketPath = protocol::buildSocketPath(directory, areaName);
    }

    config.setupTimeout = values.get("client.setup_timeout_ms", config.setupTimeout);
    config.lockTimeout = values.get("client.lock_timeout_ms", config.lockTimeout);

    refuseUnusableConfig(config, values.origin(), published.origin());
    return config;
}

}  // namespace cmed
