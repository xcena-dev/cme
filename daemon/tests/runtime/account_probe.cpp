// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// account_probe.cpp -- the text a config names an account with, and the id the kernel checks.
//
// Every case here is a refusal whose alternative is silent. A blank entry, a name nobody has and a
// digit string past the id's width each have a wrong answer that reads as root.
//
// Which accounts a host carries is the host's, so a case that needs one reports itself skipped
// rather than failing.

#include <grp.h>
#include <pwd.h>

#include <string>

#include "cmed/errors.hpp"
#include "shared/posix/account.hpp"
#include "tests/probe_context.hpp"

namespace
{

// The keys the daemon's config reader passes, since a refusal quotes the key and a reader has to be
// told which entry held the account it could not use.
constexpr const char* UidKey = "admit.uids";
constexpr const char* GidKey = "admit.gids";

// No host carries this, and one that gained it would fail the cases below rather than pass quietly.
constexpr const char* AbsentAccount = "cmed-probe-no-such-account";

// Whether @body refused, and whether the refusal names both the key and the text it could not use.
template <typename T_Body>
[[nodiscard]] bool refusesQuoting(const std::string& key, const std::string& written, T_Body body)
{
    try
    {
        body();
    }
    catch (const cmed::CmedInvalidArgumentError& refused)
    {
        const std::string said{refused.what()};
        return said.find(key) != std::string::npos && said.find(written) != std::string::npos;
    }
    return false;
}

void aNumberPassesThroughAsItself(probe::Context& ctx)
{
    ctx.openCase("a number is the id it spells");

    ctx.check(cmed::posix::getUid("0", UidKey) == 0, "zero is the id zero and not an unread entry");
    ctx.check(cmed::posix::getUid("1000", UidKey) == 1000, "a uid is the number written");
    ctx.check(cmed::posix::getGid("1000", GidKey) == 1000, "and a gid is read the same way");

    // The widest value the type holds, so the case that refuses one past it is refusing the width
    // rather than largeness.
    ctx.check(cmed::posix::getUid("4294967295", UidKey) == 4294967295U, "up to the id type's ceiling");
}

void aNameGoesToThePasswordDatabase(probe::Context& ctx)
{
    const ::passwd* const account = ::getpwnam("root");
    if (account == nullptr)
    {
        ctx.openCase("a name in the password database: skipped, this host has no root account");
        return;
    }

    ctx.openCase("a name in the password database");
    ctx.checkf(cmed::posix::getUid("root", UidKey) == account->pw_uid,
               "root resolves to the uid the database holds (%u)", account->pw_uid);
}

void aNameGoesToTheGroupDatabase(probe::Context& ctx)
{
    const ::group* const named = ::getgrnam("root");
    if (named == nullptr)
    {
        ctx.openCase("a name in the group database: skipped, this host has no root group");
        return;
    }

    ctx.openCase("a name in the group database");
    ctx.checkf(cmed::posix::getGid("root", GidKey) == named->gr_gid,
               "root resolves to the gid the database holds (%u)", named->gr_gid);
}

void anAccountNobodyCarriesIsRefused(probe::Context& ctx)
{
    ctx.openCase("a name that resolves nowhere");

    ctx.check(refusesQuoting(UidKey, AbsentAccount,
                             []
                             {
                                 static_cast<void>(cmed::posix::getUid(AbsentAccount, UidKey));
                             }),
              "an absent account is refused, and the refusal names the key and the account");
    ctx.check(refusesQuoting(GidKey, AbsentAccount,
                             []
                             {
                                 static_cast<void>(cmed::posix::getGid(AbsentAccount, GidKey));
                             }),
              "and an absent group the same way");
}

void aBlankEntryIsRefusedRatherThanRead(probe::Context& ctx)
{
    ctx.openCase("a blank entry");

    // Empty is not numeric, so this refusal is the one standing between a blank key and id zero.
    ctx.check(refusesQuoting(UidKey, "",
                             []
                             {
                                 static_cast<void>(cmed::posix::getUid("", UidKey));
                             }),
              "an empty uid entry is refused rather than read as root");
    ctx.check(refusesQuoting(GidKey, "",
                             []
                             {
                                 static_cast<void>(cmed::posix::getGid("", GidKey));
                             }),
              "and an empty gid entry the same way");
}

void aNumberTheIdCannotHoldIsRefused(probe::Context& ctx)
{
    ctx.openCase("a number past the id's width");

    // 4294967296 cast into 32 bits is zero, which is root, so narrowing here would widen access
    // rather than fail.
    ctx.check(refusesQuoting(UidKey, "4294967296",
                             []
                             {
                                 static_cast<void>(cmed::posix::getUid("4294967296", UidKey));
                             }),
              "one past the ceiling is refused rather than narrowed");
    ctx.check(refusesQuoting(GidKey, "4294967296",
                             []
                             {
                                 static_cast<void>(cmed::posix::getGid("4294967296", GidKey));
                             }),
              "and a gid past its own ceiling the same way");
    ctx.check(refusesQuoting(UidKey, "99999999999999999999",
                             []
                             {
                                 static_cast<void>(cmed::posix::getUid("99999999999999999999", UidKey));
                             }),
              "and a string of digits far past it");
}

void textThatOnlyLooksNumericIsANameNoHostHas(probe::Context& ctx)
{
    ctx.openCase("text that only looks like a number");

    // A minus sign is not a digit, so this goes to the database as a name. Read as a number instead
    // it would be the highest id there is.
    ctx.check(refusesQuoting(UidKey, "-1",
                             []
                             {
                                 static_cast<void>(cmed::posix::getUid("-1", UidKey));
                             }),
              "-1 is looked up as a name rather than read as the top id");
    ctx.check(refusesQuoting(UidKey, "1000 ",
                             []
                             {
                                 static_cast<void>(cmed::posix::getUid("1000 ", UidKey));
                             }),
              "a trailing space makes it a name, and no account carries one");
    ctx.check(refusesQuoting(UidKey, "0x3e8",
                             []
                             {
                                 static_cast<void>(cmed::posix::getUid("0x3e8", UidKey));
                             }),
              "and so does a hex spelling");
}

}  // namespace

int main()
{
    return probe::run("account probe",
                      [](probe::Context& ctx)
                      {
                          aNumberPassesThroughAsItself(ctx);
                          aNameGoesToThePasswordDatabase(ctx);
                          aNameGoesToTheGroupDatabase(ctx);
                          anAccountNobodyCarriesIsRefused(ctx);
                          aBlankEntryIsRefusedRatherThanRead(ctx);
                          aNumberTheIdCannotHoldIsRefused(ctx);
                          textThatOnlyLooksNumericIsANameNoHostHas(ctx);
                      });
}
