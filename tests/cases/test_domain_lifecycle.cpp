// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_domain_lifecycle.cpp -- the create/delete domain registry over the control domain:
//   - createDomain makes a name lockable; getDomainNames reflects it
//   - duplicate name -> DomainExistsError
//   - slot ceiling reached -> DomainLimitError
//   - deleteDomain (sole participant) removes the name; lock then UnknownDomain
//   - the freed entry is reusable -> a later createDomain succeeds
//   - deleteDomain refused while another peer still participates
//   - after that peer leaves, the sole participant can delete
//   - unknown name -> UnknownDomainError
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.

#include <cstdlib>
#include <string>
#include <vector>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

bool hasName(const std::vector<std::string>& names, const std::string& wanted)
{
    for (const auto& name : names)
    {
        if (name == wanted)
        {
            return true;
        }
    }
    return false;
}

void runBody(harness::TestContext& ctx)
{
    const std::string& uri = ctx.uri();

    // Ceiling = control(0) + 3 data slots.
    cme::Session::FormatOpts_t formatOpts;
    formatOpts.maxDomains = 4;
    formatOpts.maxPeers = 4;
    formatOpts.strategy = cme::Strategy::Request;
    cme::Session::format(uri, formatOpts);

    auto owner = cme::Session::open(uri);  // peer 0, control genesis holder

    // ── create makes a name lockable (opt-in: join before lock) ───
    owner.createDomain("inv");
    owner.joinDomain("inv");
    {
        auto guard = owner.lock("inv");
        if (!ctx.check(static_cast<bool>(guard), "lock(inv) after create + join"))
        {
            return;
        }
    }
    if (!ctx.check(hasName(owner.getDomainNames(), "inv"), "getDomainNames lists inv"))
    {
        return;
    }
    if (!ctx.check(owner.getDomainNames().size() == 1, "one data domain after first create"))
    {
        return;
    }

    // ── duplicate name refused ────────────────────────────────────
    const bool duplicateRefused = harness::threw<cme::DomainExistsError>(
        [&]
        {
            owner.createDomain("inv");
        });
    if (!ctx.check(duplicateRefused, "createDomain(inv) twice -> DomainExistsError"))
    {
        return;
    }

    // ── fill the ceiling, then overflow ───────────────────────────
    owner.createDomain("jobs");
    owner.createDomain("cache");
    if (!ctx.check(owner.getDomainNames().size() == 3, "three data domains at ceiling"))
    {
        return;
    }
    const bool overflowRefused = harness::threw<cme::DomainLimitError>(
        [&]
        {
            owner.createDomain("extra");
        });
    if (!ctx.check(overflowRefused, "createDomain past ceiling -> DomainLimitError"))
    {
        return;
    }

    // ── delete (sole participant) frees the name ──────────────────
    // deleteDomain acquires the domain lock, so the deleter must participate.
    owner.joinDomain("jobs");
    owner.deleteDomain("jobs");
    if (!ctx.check(!hasName(owner.getDomainNames(), "jobs"),
                   "deleted name gone from getDomainNames"))
    {
        return;
    }
    if (!ctx.check(owner.getDomainNames().size() == 2, "two data domains after delete"))
    {
        return;
    }
    const bool lockAfterDeleteRefused = harness::threw<cme::UnknownDomainError>(
        [&]
        {
            (void)owner.lock("jobs");
        });
    if (!ctx.check(lockAfterDeleteRefused, "lock(jobs) after delete -> UnknownDomainError"))
    {
        return;
    }

    // ── freed entry is reusable ───────────────────────────────────
    owner.createDomain("audit");  // reuses the entry "jobs" vacated
    owner.joinDomain("audit");
    if (!ctx.check(hasName(owner.getDomainNames(), "audit"), "recreated name present"))
    {
        return;
    }
    {
        auto guard = owner.lock("audit");
        if (!ctx.check(static_cast<bool>(guard), "lock(audit) after recreate"))
        {
            return;
        }
    }

    // ── delete refused while another peer participates ────────────
    auto joiner = cme::Session::open(uri);  // peer 1
    joiner.joinDomain("inv");               // opt-in: joiner now a participant of inv
    const bool deleteWithParticipantRefused = harness::threw<cme::NotParticipatingError>(
        [&]
        {
            owner.deleteDomain("inv");
        });
    if (!ctx.check(deleteWithParticipantRefused,
                   "deleteDomain(inv) with another participant -> refused"))
    {
        return;
    }

    // ── after the other peer leaves, sole participant can delete ──
    joiner.leaveDomain("inv");
    owner.deleteDomain("inv");  // owner now sole participant
    if (!ctx.check(!hasName(owner.getDomainNames(), "inv"),
                   "inv deleted after the other participant left"))
    {
        return;
    }

    // ── unknown name ──────────────────────────────────────────────
    const bool unknownDeleteRefused = harness::threw<cme::UnknownDomainError>(
        [&]
        {
            owner.deleteDomain("ghost");
        });
    if (!ctx.check(unknownDeleteRefused, "deleteDomain(ghost) -> UnknownDomainError"))
    {
        return;
    }
}

}  // namespace

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
