// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_failpoint.hpp -- killing one forked child at a named boundary.
//
// arm() is per process and SIGKILL takes the process, so the peer that must die has to be a process
// of its own. Every failpoint case was writing that same fork-arm-reap around a different body.
//
// Its own header because of what it drags in: helper_process.hpp deliberately knows no cme, and
// this one names a cme type.

#pragma once

#include <cstdint>

#include "helper_process.hpp"
#include "observe/failpoint.hpp"

namespace harness
{

// Run @body in a forked child with @boundary armed, and wait for the child. The parent never arms,
// so the kill takes the child alone. Which boundary the child reaches is decided by what @body
// does, which is why the body is the case's to write and not this function's.
//
// Returns whether the child actually died there. A body that never reaches the armed boundary
// leaves cleanly instead, and the recovery a case then observes is the recovery of a peer that
// departed -- which every policy passes, for the wrong reason.
template <typename T_Body>
[[nodiscard]] bool killChildAt(cme::failpoint::Boundary boundary, T_Body body)
{
    if (spawnChildren(
            1,
            [boundary, &body](std::uint32_t)
            {
                cme::failpoint::arm(boundary);
                body();
            }) != 1)
    {
        return false;  // fork failed; there is nothing to reap and nothing was killed
    }
    return reapChildSignalled();
}

}  // namespace harness
