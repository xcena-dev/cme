// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/protocol/socket_path.hpp -- where the two ends meet.
//
// Beside the messages rather than in them: the composition rule is a contract both ends have to agree
// on, and a requester that joins the two halves differently reaches a path nobody bound.

#pragma once

#include <string>

namespace cmed::protocol
{

// No defaults. Where the socket lives is the deployment's, and only the rule for joining is this
// build's: a path that read as "not started yet" would hide a deployment naming the wrong directory.
[[nodiscard]] inline std::string buildSocketPath(const std::string& directory, const std::string& areaName)
{
    return directory + "/" + areaName + ".sock";
}

}  // namespace cmed::protocol
