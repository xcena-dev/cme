// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_held_domain_refusal.cpp -- a peer cannot delete or leave a domain it still holds a Guard on.
//
// Locks re-enter per peer, so neither call would block on the held Guard. Both clear the domain's
// pins, and the Guard released afterwards would then free a turn that belongs to a later lock.

#include <cstdint>
#include <utility>

#include "cme/errors.hpp"
#include "core/types.hpp"
#include "helper_cme.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

constexpr std::uint32_t FormatDomains = 2;  // control + one data slot
constexpr std::uint32_t FormatPeers = 2;

constexpr cme::PeerId OwnerId = 0;
constexpr cme::PeerId OtherId = 1;

template <typename T>
[[nodiscard]] bool refusesAsBusy(T&& body)
{
    try
    {
        std::forward<T>(body)();
    }
    catch (const cme::DomainBusyError&)
    {
        return true;
    }
    return false;
}

void checkDelete(harness::TestContext& ctx)
{
    auto region = harness::createRegion(FormatDomains, FormatPeers);
    auto owner = harness::makePeer(region, OwnerId);
    const auto domain = owner.createDomain("held");
    owner.joinDomain(domain.id);
    const auto deleteDomain = [&]
    {
        owner.deleteDomain(domain.id);
    };

    {
        auto held = owner.lock(domain.id);
        ctx.check(refusesAsBusy(deleteDomain), "delete refuses a domain this peer still holds");
    }
    ctx.check(!refusesAsBusy(deleteDomain), "delete goes through once the guard is released");
}

void checkLeave(harness::TestContext& ctx)
{
    auto region = harness::createRegion(FormatDomains, FormatPeers);
    auto owner = harness::makePeer(region, OwnerId);
    auto other = harness::makePeer(region, OtherId);
    const auto domain = owner.createDomain("held");
    owner.joinDomain(domain.id);
    other.joinDomain(domain.id);
    const auto leaveDomain = [&]
    {
        other.leaveDomain(domain.id);
    };

    {
        auto held = other.lock(domain.id);
        ctx.check(refusesAsBusy(leaveDomain), "leave refuses a domain this peer still holds");
    }
    ctx.check(!refusesAsBusy(leaveDomain), "leave goes through once the guard is released");
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    checkDelete(ctx);
    checkLeave(ctx);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
