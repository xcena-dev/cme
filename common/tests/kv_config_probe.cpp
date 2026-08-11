// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// kv_config_probe.cpp -- the YAML subset, held to what its header promises.
//
// The cases that matter are the ones where a lenient reader would quietly agree with a file that
// says something else: a hash inside a value, a colon inside a value, a key whose blank value is a
// setting rather than a section, and a number that is not one.

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "common/kv_config.hpp"

namespace
{

kvconfig::KeyValueConfig read(const std::string& text)
{
    std::istringstream input{text};
    return kvconfig::KeyValueConfig::parse(input, "probe.yaml");
}

bool nestingBecomesAPath()
{
    const auto config = read(
        "region:\n"
        "  uri: shm:/cme-region\n"
        "  coherency: uncached\n"
        "area:\n"
        "  path: /cmed\n");

    // The leaf name alone would collide here; the path is what keeps the two apart.
    return config.getString("region.uri") == "shm:/cme-region" &&
           config.getString("region.coherency") == "uncached" && config.getString("area.path") == "/cmed" &&
           !config.has("uri") && !config.has("path");
}

bool dedentClosesTheSection()
{
    const auto config = read(
        "region:\n"
        "  uri: shm:/one\n"
        "workers: 4\n");

    return config.getU64("workers", 0) == 4 && !config.has("region.workers");
}

// A blank value is a section only when deeper lines follow. Both readings have to work, and which
// one applies is not visible until the next line.
bool aBlankValueIsBothShapes()
{
    const auto config = read(
        "area:\n"
        "  group:\n"
        "  mode: 0660\n");

    return config.getString("area.group") == "" && config.getString("area.mode") == "0660" &&
           config.getString("area.group.mode", "absent") == "absent";
}

bool commentsStopAtTheValue()
{
    const auto config = read(
        "uri: shm:/area#0    # trailing note\n"
        "note: \"a # inside quotes\"\n"
        "#whole line\n");

    return config.getString("uri") == "shm:/area#0" && config.getString("note") == "a # inside quotes" &&
           !config.has("#whole line");
}

bool aValueKeepsItsOwnColons()
{
    return read("uri: dax:/dev/dax0.0\n").getString("uri") == "dax:/dev/dax0.0";
}

bool inlineSequencesSplit()
{
    const auto config = read(
        "hosts: [alpha, \"beta gamma\", delta]\n"
        "empty: []\n");

    const std::vector<std::string> hosts = config.getList("hosts");
    return hosts.size() == 3 && hosts[0] == "alpha" && hosts[1] == "beta gamma" && hosts[2] == "delta" &&
           config.getList("empty").empty() && config.getList("absent").empty();
}

bool boolsTakeTheirSpellings()
{
    const auto config = read("one: yes\ntwo: off\n");
    return config.getBool("one", false) && !config.getBool("two", true) && config.getBool("absent", true);
}

template <typename T>
bool refuses(T&& body)
{
    try
    {
        body();
    }
    catch (const kvconfig::ParseError&)
    {
        return true;
    }
    return false;
}

// Each of these would otherwise read as a default while the file says something else, and nothing
// afterwards would point back at the line.
bool refusesWhatItCannotRead()
{
    // The casts say the calls are made for their throw, not for what they return.
    return refuses([]
                   {
                       static_cast<void>(read("workers 4\n"));
                   }) &&  // no colon
           refuses([]
                   {
                       static_cast<void>(read("region:\n\turi: x\n"));
                   }) &&  // tab indent
           refuses([]
                   {
                       static_cast<void>(read(": 4\n"));
                   }) &&  // empty key
           refuses([]
                   {
                       static_cast<void>(read("count: many\n").getU64("count", 0));
                   }) &&  // not a number
           refuses([]
                   {
                       static_cast<void>(read("on: maybe\n").getBool("on", false));
                   }) &&  // not a boolean
           refuses([]
                   {
                       static_cast<void>(read("hosts: alpha, beta\n").getList("hosts"));
                   });  // not a sequence
}

bool anAbsentFileIsAllFallbacks()
{
    const auto config = kvconfig::KeyValueConfig::loadIfPresent("/nonexistent/cme/probe.yaml");
    return config.getU64("workers", 7) == 7 && config.getString("area.path", "none") == "none" &&
           refuses([]
                   {
                       return kvconfig::KeyValueConfig::load("/nonexistent/cme/probe.yaml");
                   });
}

}  // namespace

int main()
{
    bool passed = false;
    try
    {
        passed = nestingBecomesAPath() && dedentClosesTheSection() && aBlankValueIsBothShapes() &&
                 commentsStopAtTheValue() && aValueKeepsItsOwnColons() && inlineSequencesSplit() &&
                 boolsTakeTheirSpellings() && refusesWhatItCannotRead() && anAbsentFileIsAllFallbacks();
    }
    catch (const kvconfig::ParseError& refusal)
    {
        // A case above refused input it was written to accept, which is a finding rather than a
        // crash, so it is reported the same way a false answer is.
        std::printf("kv config probe threw: %s\n", refusal.what());
        return 1;
    }

    std::printf("kv config probe %s\n", passed ? "ok" : "failed");
    return passed ? 0 : 1;
}
