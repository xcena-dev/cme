// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_domain_handle.cpp -- a handle acquires the domain it named, or nothing.
//
// resolveDomain walks the region once and hands back the slot with the incarnation it carried. The
// point of carrying it is what this case drives: a slot freed and claimed again keeps its index, so an
// index alone would acquire whatever took the slot and answer as if nothing had happened.
//
// domain_incarnation covers the field itself rising on reuse. What is here is the API that reads it:
// the refusal is a property of lock and tryLock, and a build that dropped the comparison would still
// pass that case.
//
// The region has one data slot, so every create after the first lands on the previous domain's bytes.
// The last round recreates under the same name, which is the case a name comparison cannot answer.
//
// Session rather than Peer: the handle is a Session-level answer, and the comparison it exists for
// happens inside Session::lock.

#include <cstdint>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

// Slot 0 is control and slot 1 is the only data slot, so a reuse cannot land anywhere else.
constexpr std::uint32_t FormatDomains = 2;
constexpr std::uint32_t FormatPeers = 2;

constexpr const char* FirstName = "alpha";
constexpr const char* SecondName = "beta";

constexpr timing::Millis GrantWindow{3'000};

}  // namespace

void runBody(harness::TestContext& ctx)
{
    harness::formatSession(FormatDomains, FormatPeers);

    cme::Session holder = harness::openSession();
    cme::Session tenant = harness::openSession();

    holder.createDomain(FirstName);
    tenant.joinDomain(FirstName);

    const cme::DomainHandle_t named = tenant.resolveDomain(FirstName);
    if (!ctx.check(harness::canLock(tenant, named, GrantWindow),
                   "the handle acquires the domain it named"))
    {
        return;
    }

    // Out before the delete: cme refuses one while a peer other than the deleter participates.
    tenant.leaveDomain(FirstName);
    holder.deleteDomain(FirstName);
    holder.createDomain(SecondName);

    const cme::DomainHandle_t reused = tenant.resolveDomain(SecondName);
    if (!ctx.checkf(reused.id == named.id, "the new domain lands on slot %u again", named.id))
    {
        return;
    }
    ctx.check(reused.incarnation != named.incarnation, "and carries a different incarnation");

    // Joined before the stale handle is tried, and that is the whole point: not participating would
    // refuse the acquire on its own, and the case would pass against a build that never compared
    // incarnations. Participating, that comparison is the only thing left.
    tenant.joinDomain(SecondName);

    const auto lockReused = [&tenant, named]
    {
        static_cast<void>(tenant.lock(named));
    };
    ctx.check(harness::threw<cme::UnknownDomainError>(lockReused),
              "the old handle is refused rather than served the domain that took the slot");

    const auto tryLockReused = [&tenant, named]
    {
        static_cast<void>(tenant.tryLock(named, GrantWindow));
    };
    ctx.check(harness::threw<cme::UnknownDomainError>(tryLockReused),
              "and tryLock refuses it too, rather than answering nullopt as if it had timed out");

    // A handle nobody resolved. Zero is what a free slot reads, so a build comparing the two words alone
    // lets this one through to a slot holding no domain.
    const auto lockZero = [&tenant, named]
    {
        static_cast<void>(tenant.lock(cme::DomainHandle_t{named.id, 0}));
    };
    ctx.check(harness::threw<cme::UnknownDomainError>(lockZero),
              "a handle carrying no incarnation is refused");

    // An id past the table, with an incarnation that would match if the read were believed. The record
    // lookup is index arithmetic, so an unchecked id reads bytes outside the region before anything
    // rejects it.
    const auto lockPastTable = [&tenant, reused]
    {
        static_cast<void>(tenant.lock(cme::DomainHandle_t{FormatDomains + 7, reused.incarnation}));
    };
    ctx.check(harness::threw<cme::UnknownDomainError>(lockPastTable),
              "a handle naming a slot past the table is refused without reading it");

    // Far enough that the index times the record stride leaves the mapping entirely, which the earlier
    // one need not: a slot just past the table can still land on bytes the region owns.
    const auto lockFarPast = [&tenant, reused]
    {
        static_cast<void>(tenant.lock(cme::DomainHandle_t{0xFFFF'0000U, reused.incarnation}));
    };
    ctx.check(harness::threw<cme::UnknownDomainError>(lockFarPast),
              "and so is one far past it, rather than faulting on the read");

    ctx.check(harness::canLock(tenant, reused, GrantWindow),
              "the handle resolved after the reuse acquires it");

    // The case the name cannot carry: same slot, same name, different incarnation. A build that compared
    // names instead of incarnations passes everything above and fails here.
    tenant.leaveDomain(SecondName);
    holder.deleteDomain(SecondName);
    holder.createDomain(SecondName);
    tenant.joinDomain(SecondName);

    const cme::DomainHandle_t renamed = tenant.resolveDomain(SecondName);
    if (!ctx.checkf(renamed.id == reused.id, "the same name lands on slot %u a third time", reused.id))
    {
        return;
    }
    ctx.check(renamed.incarnation != reused.incarnation,
              "and the incarnation is the only word that says it is a different domain");

    const auto lockRenamed = [&tenant, reused]
    {
        static_cast<void>(tenant.lock(reused));
    };
    ctx.check(harness::threw<cme::UnknownDomainError>(lockRenamed),
              "the handle from before the recreate is refused despite naming the same string");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
