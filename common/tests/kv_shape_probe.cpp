// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// kv_shape_probe.cpp -- the shapes a file arrives in, rather than the values it carries.
//
// kv_config_probe.cpp holds the parser to what its header promises about values. What is left is
// the file's own form: how deep the nesting may go, what a second copy of a key means, and what a
// carriage return at the end of every line does to a reader that trims and compares. Each of these
// has a wrong answer that looks like a correct one, because a path built one letter off and a value
// carrying an invisible \r both read as a setting that is simply absent.

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "common/kv_config.hpp"
#include "tests/probe_context.hpp"

namespace
{

kvconfig::KeyValueConfig read(const std::string& text)
{
    std::istringstream input{text};
    return kvconfig::KeyValueConfig::parse(input, "shape.yaml");
}

// Two levels are the shallowest nesting that can go wrong, and the existing probe covers those.
// Three is where a section stack that pops one frame too few or too many first shows itself.
void nestingGoesThreeDeep(probe::Context& ctx)
{
    ctx.openCase("nesting three deep");

    const auto config = read(
        "daemon:\n"
        "  area:\n"
        "    path: /cmed\n"
        "    mode: 0660\n"
        "  workers: 4\n"
        "log:\n"
        "  path: /var/log/cmed\n");

    ctx.check(config.getString("daemon.area.path") == "/cmed", "the deepest key carries its whole path");
    ctx.check(config.getString("daemon.area.mode") == "0660", "and so does its sibling");
    ctx.check(config.get("daemon.workers", std::uint64_t{0}) == 4, "one dedent closes the inner section only");
    ctx.check(config.getString("log.path") == "/var/log/cmed", "a dedent to column zero opens a new one");
    // The two leaf names collide, and the path is the only thing keeping them apart.
    ctx.check(!config.has("path") && !config.has("area.path"), "no leaf name escapes its section");
    ctx.check(!config.has("daemon.area.workers"), "and no key lands under a section it left");
}

// A file written on another machine arrives with \r before every newline. The reader trims it out
// of both halves, so a value must not end up one invisible letter longer than the file reads.
void carriageReturnsAreNotPartOfAValue(probe::Context& ctx)
{
    ctx.openCase("CRLF line endings");

    const auto config = read(
        "region:\r\n"
        "  uri: shm:/cme-region\r\n"
        "  coherency: uncached\r\n"
        "workers: 4\r\n");

    ctx.check(config.getString("region.uri") == "shm:/cme-region", "the value stops before the \\r");
    ctx.check(config.getString("region.coherency") == "uncached", "and so does the next one");
    ctx.check(config.get("workers", std::uint64_t{0}) == 4, "a number is still a number");
    // The dedent still closes the section, which it would not if \r were counted as a letter of the
    // line and the section stack were popped against a different column.
    ctx.check(!config.has("region.workers"), "the dedent still closes the section");
}

// The map holds one entry per path, so the second line is the one that survives. That is the rule
// a caller relies on when a generated file appends an override to the end.
void aRepeatedKeyTakesItsLastValue(probe::Context& ctx)
{
    ctx.openCase("a key that appears twice");

    const auto config = read(
        "workers: 4\n"
        "workers: 9\n"
        "region:\n"
        "  uri: shm:/first\n"
        "region:\n"
        "  uri: shm:/second\n");

    ctx.check(config.get("workers", std::uint64_t{0}) == 9, "the last value wins at the top level");
    ctx.check(config.getString("region.uri") == "shm:/second", "and inside a reopened section");
}

// The value is split on commas and nothing else, so a colon inside an item stays in that item. A
// reader that looked for the last colon on the line would cut the first item in half.
void aSequenceKeepsColonsInsideItsItems(probe::Context& ctx)
{
    ctx.openCase("a colon inside an inline sequence");

    const auto config = read(
        "uris: [shm:/cme-region, dax:/dev/dax0.0, /plain/path]\n"
        "single: [shm:/only]\n");

    const std::vector<std::string> uris = config.getList("uris");
    if (!ctx.check(uris.size() == 3, "the sequence splits into three items"))
    {
        return;
    }

    ctx.check(uris[0] == "shm:/cme-region", "the first item keeps its colon");
    ctx.check(uris[1] == "dax:/dev/dax0.0", "the second keeps its colon and its dots");
    ctx.check(uris[2] == "/plain/path", "and an item with no colon is untouched");
    ctx.check(config.getList("single").size() == 1 && config.getList("single")[0] == "shm:/only",
              "a one-item sequence is one item");
}

// A comment carries no indentation of its own as far as the section stack is concerned, because it
// is dropped before the column is measured. A reader that measured first would close the section.
void aCommentDoesNotCloseASection(probe::Context& ctx)
{
    ctx.openCase("a comment at column zero inside a section");

    const auto config = read(
        "region:\n"
        "  uri: shm:/one\n"
        "# back at column zero\n"
        "  coherency: uncached\n");

    ctx.check(config.getString("region.uri") == "shm:/one", "the key before the comment is in the section");
    ctx.check(config.getString("region.coherency") == "uncached", "and so is the key after it");
    ctx.check(!config.has("coherency"), "the comment did not close the section");
}

}  // namespace

int main()
{
    return probe::run("kv shape probe",
                      [](probe::Context& ctx)
                      {
                          nestingGoesThreeDeep(ctx);
                          carriageReturnsAreNotPartOfAValue(ctx);
                          aRepeatedKeyTakesItsLastValue(ctx);
                          aSequenceKeepsColonsInsideItsItems(ctx);
                          aCommentDoesNotCloseASection(ctx);
                      });
}
