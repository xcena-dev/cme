// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// claim.hpp -- peer-slot claim (Session::open path).
//
// Atomic-free: joins serialise through a nonce lease on the admission-control
// line (stake/settle/re-read, steal-on-stall), so the claimer reserves the lowest
// free slot and grows peerScanBound as the lease's sole writer.

#pragma once

#include "core/types.hpp"

namespace cme
{
class Geometry;  // fwd
}

namespace cme::admission
{

// Claim a free peer slot in the membership table. Throws NoFreeSlotError when the
// lease cannot be acquired before LeaseAcquireDeadline or the table is full.
// Runs under the mode @geometry's mapping calls for.
[[nodiscard]] PeerId claimPeerSlot(const Geometry& geometry);

}  // namespace cme::admission
