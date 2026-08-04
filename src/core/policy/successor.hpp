// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// successor.hpp -- the cross-strategy entry points over the SuccessorPolicy kinds.
//
// Which concrete policies exist is the .cpp's business. A caller here names a Strategy and
// gets back the abstract base, so including this header does not drag in four policy headers
// (and through them their layouts) for the sake of one switch.

#pragma once

#include <cstdint>
#include <memory>

#include "cme/shared.hpp"
#include "core/policy/successor_policy.hpp"

namespace cme
{

// Factory: build the concrete policy for @kind. nullptr for an unimplemented strategy.
[[nodiscard]] std::unique_ptr<SuccessorPolicy> makeSuccessorPolicy(Strategy kind);

// Per-strategy trailing FAM area bytes, delegated to the policy's own sizing
// (SuccessorPolicy::getRegionAreaSize). Builds a throwaway policy -- only called
// on format/bind, never the hot path. 0 for an unimplemented strategy.
[[nodiscard]] std::uint64_t getSuccessorAreaSize(Strategy strategy, std::uint32_t domainCount,
                                                 std::uint32_t peerCount,
                                                 std::uint32_t aggregatorGroups) noexcept;

}  // namespace cme
