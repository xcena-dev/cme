// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// The cross-strategy entry points. Defined here rather than inline so that including
// successor.hpp costs the abstract base alone, not every concrete policy header.

#include "core/policy/successor.hpp"

#include <cstdint>
#include <memory>

#include "cme/shared.hpp"
#include "core/policy/successor_order.hpp"
#include "core/policy/successor_peterson.hpp"
#include "core/policy/successor_policy.hpp"
#include "core/policy/successor_request.hpp"
#include "core/policy/successor_request_agg.hpp"

namespace cme
{

std::unique_ptr<SuccessorPolicy> makeSuccessorPolicy(Strategy kind)
{
    switch (kind)
    {
        case Strategy::Order:
            return std::make_unique<OrderPolicy>();
        case Strategy::Request:
            return std::make_unique<RequestPolicy>();
        case Strategy::RequestAgg:
            return std::make_unique<RequestAggPolicy>();
        case Strategy::Peterson:
            return std::make_unique<PetersonPolicy>();
    }
    return nullptr;
}

std::uint64_t getSuccessorAreaSize(Strategy strategy, std::uint32_t domainCount,
                                   std::uint32_t peerCount,
                                   std::uint32_t aggregatorGroups) noexcept
{
    const std::unique_ptr<SuccessorPolicy> policy = makeSuccessorPolicy(strategy);
    return (policy != nullptr) ? policy->getRegionAreaSize(domainCount, peerCount, aggregatorGroups)
                               : 0;
}

}  // namespace cme
