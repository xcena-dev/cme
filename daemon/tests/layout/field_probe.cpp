// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// field_probe.cpp -- a domain name in its field, at the edges a happy write never reaches.
//
// abi_probe asserts the field's width. This asserts what a write to one leaves in it, because such a
// field is compared and hashed whole: the tail a short write leaves behind is part of the value.
//
// The ceiling is one below the width, because cme reads the field as NUL-terminated. A name filling
// it would read back whole here and one letter short there.

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#include "shared/protocol/domain_name.hpp"
#include "tests/probe_context.hpp"

namespace
{

constexpr std::uint32_t FieldWidth = cmed::protocol::DomainName::FieldWidth;

// Exactly FieldWidth letters, which would leave the field with no terminator at all.
constexpr std::string_view FullText = "0123456789abcdef";

// One letter short of the field, which is the longest name a write takes.
constexpr std::string_view LongestText = "0123456789abcde";

// One letter past the field. Cutting it would store the name of something else.
constexpr std::string_view PastText = "0123456789abcdefg";

// Leaves the field alone for a text no name could be made of, which is what a case checking that a
// refusal changes nothing needs.
void writeOrLeave(char (&field)[FieldWidth], std::string_view text)
{
    if (const auto named = cmed::protocol::DomainName::make(text))
    {
        named->write(field);
    }
}

void aTextFillingTheFieldIsRefused(probe::Context& ctx)
{
    ctx.openCase("a text that exactly fills the field");

    ctx.check(!cmed::protocol::DomainName::make(FullText), "a text of the field's own width is refused");

    char field[FieldWidth] = {};
    writeOrLeave(field, LongestText);
    ctx.check(cmed::protocol::DomainName::read(field) == LongestText, "one letter shorter is taken, and reads back whole");
    ctx.check(field[FieldWidth - 1] == '\0', "and the last byte is the terminator its readers stop at");
}

void aShorterTextZeroesTheTail(probe::Context& ctx)
{
    ctx.openCase("the tail a shorter text leaves");

    char reused[FieldWidth] = {};
    writeOrLeave(reused, LongestText);
    writeOrLeave(reused, "ab");

    char fresh[FieldWidth] = {};
    writeOrLeave(fresh, "ab");

    // One text is one value, so the field written over a longer one has to equal the field written
    // over zeros. Compared whole, since that is how a caller compares two of these.
    ctx.check(std::memcmp(reused, fresh, FieldWidth) == 0, "and leaves the same bytes a fresh field would");
    ctx.check(cmed::protocol::DomainName::read(reused) == "ab", "so the field names the shorter text alone");
}

void aTextPastTheFieldIsRefusedAndChangesNothing(probe::Context& ctx)
{
    ctx.openCase("a text one letter past the field");

    char field[FieldWidth] = {};
    writeOrLeave(field, "abc");

    ctx.check(!cmed::protocol::DomainName::make(PastText), "a text past the width is refused rather than cut");
    writeOrLeave(field, PastText);
    ctx.check(cmed::protocol::DomainName::read(field) == "abc", "and the field still holds what it held before");
}

void anEmptyFieldIsHowAFieldSaysItHoldsNothing(probe::Context& ctx)
{
    ctx.openCase("the empty text, and the empty field");

    char field[FieldWidth] = {};
    ctx.check(cmed::protocol::DomainName::read(field) == "", "a field of zeros reads as no text");
    ctx.check(!cmed::protocol::DomainName::make(""), "an empty text is refused, since that state is the zeros");

    writeOrLeave(field, LongestText);
    cmed::protocol::DomainName::clear(field);
    ctx.check(cmed::protocol::DomainName::read(field) == "", "clear takes a written field back to holding none");

    char fresh[FieldWidth] = {};
    ctx.check(std::memcmp(field, fresh, FieldWidth) == 0, "and zeroes every byte, not only the first");
}

// The pair does not round-trip a text carrying a NUL: the write takes it and the read stops at it.
// Nothing in the tree writes one today, and a name arriving from a caller's string_view could.
void aTextCarryingItsOwnNulComesBackShorter(probe::Context& ctx)
{
    ctx.openCase("a text with a NUL inside it");

    constexpr std::string_view Interrupted{"ab\0cd", 5};

    char field[FieldWidth] = {};
    ctx.check(cmed::protocol::DomainName::make(Interrupted).has_value(),
              "a text carrying a NUL is accepted by its length");
    writeOrLeave(field, Interrupted);
    ctx.check(cmed::protocol::DomainName::read(field) == "ab", "and comes back as the part before the NUL");
    ctx.check(cmed::protocol::DomainName::read(field) != Interrupted, "so this one write does not round-trip");
}

}  // namespace

int main()
{
    return probe::run("field probe",
                      [](probe::Context& ctx)
                      {
                          aTextFillingTheFieldIsRefused(ctx);
                          aShorterTextZeroesTheTail(ctx);
                          aTextPastTheFieldIsRefusedAndChangesNothing(ctx);
                          anEmptyFieldIsHowAFieldSaysItHoldsNothing(ctx);
                          aTextCarryingItsOwnNulComesBackShorter(ctx);
                      });
}
