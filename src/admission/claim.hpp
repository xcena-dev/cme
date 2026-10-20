// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// claim.hpp -- peer-slot claim (Session::open path).
//
// Atomic-free: joins serialise through the admission lease, whose holder alone writes the
// admission plane.

#pragma once

#include "core/types.hpp"

namespace cme
{
class Geometry;  // fwd
}

namespace cme::admission
{

// Claim a free peer slot in the membership table. Throws NoFreeSlotError when the
// lease cannot be acquired before config::lease::AcquireDeadline or the table is full.
// Runs under the mode @geometry's mapping calls for.
[[nodiscard]] PeerId claimPeerSlot(const Geometry& geometry);

}  // namespace cme::admission
