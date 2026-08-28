// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// lookup_probe.cpp -- names to ids, and the answers a caller must not read as each other.
//
// Absent, present at id zero, and present at an id that has since moved on are three states, and
// each has a reading that costs a domain: zero taken for absent, or a stale drop taking a live entry.
//
// The keys arrive as views into a record's fixed-width field, so one case keys on a view rather than
// on a C string.

#include <cstdint>
#include <optional>
#include <string_view>

#include "shared/protocol/domain_name.hpp"
#include "shared/protocol/id_lookup.hpp"
#include "tests/probe_context.hpp"

namespace
{

// Made once, since every case here asks about the same two names.
const cmed::protocol::DomainName Lane = cmed::protocol::DomainName::make("lane").value();
const cmed::protocol::DomainName Other = cmed::protocol::DomainName::make("other").value();

void anAbsentNameAnswersNothingAndIdZeroIsAnAnswer(probe::Context& ctx)
{
    ctx.openCase("nothing, against an id that is zero");

    cmed::protocol::IdLookup lookup;
    ctx.check(!lookup.find(Lane).has_value(), "a lookup told nothing answers nothing");

    lookup.set(Lane, 0);
    const std::optional<std::uint32_t> found = lookup.find(Lane);

    // A caller reading zero as absent would take domain zero for a name nobody has, which is the
    // reason this answers through an optional rather than through a sentinel.
    ctx.check(found.has_value(), "a name set at id zero is present");
    ctx.check(found.has_value() && *found == 0, "and answers zero rather than nothing");
    ctx.check(!lookup.find(Other).has_value(), "while a name never set still answers nothing");
}

void settingAgainReplacesTheId(probe::Context& ctx)
{
    ctx.openCase("the same name at a second id");

    cmed::protocol::IdLookup lookup;
    lookup.set(Lane, 3);
    lookup.set(Lane, 4);

    ctx.check(lookup.find(Lane).value_or(0) == 4, "the later set is the one that answers");
}

void clearKeepsAnEntrySetAtAnotherId(probe::Context& ctx)
{
    ctx.openCase("clearing a name that has moved on");

    cmed::protocol::IdLookup lookup;
    lookup.set(Lane, 4);

    // A name dropped from one id may have been set at another since, and it is that entry a caller
    // would lose by clearing on the name alone.
    lookup.clear(Lane, 3);
    ctx.check(lookup.find(Lane).value_or(0) == 4, "a clear naming the old id leaves the live entry");

    lookup.clear(Lane, 4);
    ctx.check(!lookup.find(Lane).has_value(), "and one naming the live id drops it");

    lookup.set(Lane, 4);
    lookup.clear(Other, 4);
    ctx.check(lookup.find(Lane).value_or(0) == 4, "clearing a name never set touches nothing");
}

void aViewKeysOnItsOwnLength(probe::Context& ctx)
{
    ctx.openCase("a name that is a view into a longer buffer");

    // What a record's fixed-width field reads back as: a view with no terminator behind it.
    constexpr std::string_view Field = "lanelane";
    const auto whole = cmed::protocol::DomainName::make(Field);
    const auto part = cmed::protocol::DomainName::make(Field.substr(0, 4));
    ctx.check(whole.has_value() && part.has_value(), "a field-width view and a shorter one both name a domain");
    if (!whole || !part)
    {
        return;
    }

    cmed::protocol::IdLookup lookup;
    lookup.set(*part, 7);

    ctx.check(lookup.find(*part).value_or(0) == 7, "the view's own extent is the key");
    ctx.check(!lookup.find(*whole).has_value(), "and the buffer behind it is not that key");
}

}  // namespace

int main()
{
    return probe::run("lookup probe",
                      [](probe::Context& ctx)
                      {
                          anAbsentNameAnswersNothingAndIdZeroIsAnAnswer(ctx);
                          settingAgainReplacesTheId(ctx);
                          clearKeepsAnEntrySetAtAnotherId(ctx);
                          aViewKeysOnItsOwnLength(ctx);
                      });
}
