// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// refusal_probe.cpp -- the daemon answers Error, and the requester has to carry the reason out.
//
// Error is the one settled answer that is not a grant. The errno the daemon left in result is the
// only thing that tells a caller why, so it has to arrive as the exception's code() rather than as
// prose inside what(). The domain also has to come back to Idle afterwards, or one refusal would
// cost every later requester the domain.

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <string>
#include <system_error>

#include "cmed/errors.hpp"
#include "cmed/session.hpp"
#include "common/timing.hpp"
#include "harness/helper.hpp"
#include "harness/helper_requester.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

constexpr const char* DomainName = "lane0";

constexpr std::uint32_t DomainSlot = cmed::harness::FirstDataSlot;

// Negative, the way the daemon writes it: result carries an errno the caller negates back.
constexpr std::int32_t Refusal = -EPERM;

// One refusal is owed per acquire the cases below make.
constexpr std::uint32_t ExpectedRefusals = 3;

// Twice, because a refusal that left the state machine or the mutex behind would let the first
// attempt pass and the second hang or throw something else.
void everyRefusalCarriesItsErrno(probe::Context& ctx, cmed::CmedSession& session, cmed::protocol::SharedArea_t& area)
{
    ctx.openCase("lock() against a daemon that answers Error");

    cmed::protocol::Domain_t& context = cmed::harness::resolveSlot(area, DomainSlot);
    for (std::uint32_t attempt = 1; attempt <= 2; ++attempt)
    {
        bool refused = false;
        bool carriedTheErrno = false;
        try
        {
            // The cast is what the call is for: this line is expected to throw, not to return.
            static_cast<void>(session.lock(DomainName));
        }
        catch (const cmed::CmedLockRefusedError& failure)
        {
            refused = true;
            carriedTheErrno = failure.code() == std::errc::operation_not_permitted;
        }

        ctx.checkf(refused, "attempt %" PRIu32 " throws CmedLockRefusedError", attempt);
        ctx.checkf(carriedTheErrno, "attempt %" PRIu32 " carries EPERM in code()", attempt);
        ctx.checkf(cmed::harness::isIdle(area, DomainSlot), "attempt %" PRIu32 " leaves the domain Idle",
                   attempt);
        ctx.checkf(cmed::harness::isMutexFree(context.request.lock), "attempt %" PRIu32 " hands the mutex back",
                   attempt);
    }
}

// tryLock answers nullopt for a deadline and nothing else. A refusal reaching a caller as an empty
// optional would read as "not yet" and be retried forever.
void aRefusalIsNotADeadline(probe::Context& ctx, cmed::CmedSession& session)
{
    ctx.openCase("tryLock against the same daemon");

    bool threw = false;
    bool answeredNullopt = false;
    try
    {
        answeredNullopt = !session.tryLock(DomainName, timing::Secs{5}).has_value();
    }
    catch (const cmed::CmedLockRefusedError&)
    {
        threw = true;
    }

    ctx.check(threw, "tryLock throws rather than returning");
    ctx.check(!answeredNullopt, "and never answers nullopt for a refusal");
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"refusal-probe"};
    return probe::run(
        "refusal probe",
        [&scratch](probe::Context& ctx)
        {
            const std::string areaName = scratch.makeAreaName("");
            cmed::harness::ProbeArea area{areaName.c_str()};
            cmed::harness::publishDomain(area.shared(), DomainSlot, DomainName);

            const cmed::harness::StubDaemon daemon{area.shared(), cmed::harness::refuseEveryLock(Refusal)};
            const cmed::harness::StubSetup setup{scratch.makePath("cmed.sock"), area.descriptor()};
            cmed::CmedSession session = setup.openRequester();

            everyRefusalCarriesItsErrno(ctx, session, area.shared());
            aRefusalIsNotADeadline(ctx, session);

            ctx.openCase("what the daemon was asked for");
            static_cast<void>(daemon.awaitRefusals(ExpectedRefusals));
            ctx.checkf(daemon.refusals() == ExpectedRefusals, "%" PRIu32 " acquires reached the daemon",
                       daemon.refusals());
            ctx.checkf(daemon.refusalsFor(DomainSlot) == ExpectedRefusals,
                       "%" PRIu32 " of them against the domain that was asked for",
                       daemon.refusalsFor(DomainSlot));

            // The revoke a refused caller never asked for. Without it the daemon would be left
            // believing the domain is still someone's.
            static_cast<void>(daemon.awaitReleases(ExpectedRefusals));
            ctx.checkf(daemon.releases() == ExpectedRefusals, "%" PRIu32 " revokes, one per refusal",
                       daemon.releases());

            // On the domain itself, since a slot nothing published reads Idle too and a revoke rung
            // at the wrong one would raise the total just the same.
            ctx.checkf(daemon.releasesFor(DomainSlot) == ExpectedRefusals,
                       "%" PRIu32 " of them at the domain the caller held", daemon.releasesFor(DomainSlot));
        });
}
