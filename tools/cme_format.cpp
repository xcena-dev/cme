// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cme_format.cpp -- lay out a cme region, once, before anything opens it.
//
// Session::format zeroes the region and lays fresh peer slots over it, and two callers running it
// at the same time are not serialised against each other. So it cannot be a daemon's start-up step:
// every daemon on every node starts at the same moment, and each would be a second formatter. The
// deliberate single act has to be a separate one, which is this.
//
// Refuses a region that already answers, unless --force. Formatting a live one discards the domains
// and peer slots its nodes are using, and nothing about the result says that is what happened.
//
// Creating domains is not part of formatting. The flag is here because a region is provisioned once
// and its domains are usually known then, and any peer can do the same at run time.
//
// --config reads the same file cmed does, under region.*, so one file describes one region: where it
// is, and the shape this program lays into it. A flag on the command line wins over the file, which
// is what lets an operator override one value without editing a deployment's own config.

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/args.hpp"
#include "common/kv_config.hpp"
#include "observe/inspector.hpp"

namespace
{

[[nodiscard]] cme::Strategy strategyFromName(const std::string& name)
{
    if (name == "order")
    {
        return cme::Strategy::Order;
    }
    if (name == "request")
    {
        return cme::Strategy::Request;
    }
    if (name == "request_agg")
    {
        return cme::Strategy::RequestAgg;
    }
    if (name == "peterson")
    {
        return cme::Strategy::Peterson;
    }

    throw cme::InvalidArgumentError{"--strategy is not a strategy: " + name};
}

// The name config carries, as the mode libcme takes. Throws rather than guessing: the wrong mode
// reads a stale header on a region no cache keeps in sync.
[[nodiscard]] cme::CoherencyMode coherencyFromName(const std::string& name)
{
    if (name == "cache_coherent")
    {
        return cme::CoherencyMode::CacheCoherent;
    }
    if (name == "uncached")
    {
        return cme::CoherencyMode::Uncached;
    }
    if (name == "flush")
    {
        return cme::CoherencyMode::Flush;
    }

    throw cme::InvalidArgumentError{"region.coherency is not a mode: " + name};
}

// True when the region answers, so formatting would take it from whoever is on it. Reads the header
// and joins nothing: whoever mounts asks this, and a region may grant that caller read alone.
[[nodiscard]] bool alreadyLive(const std::string& uri, cme::CoherencyMode coherency)
{
    try
    {
        const cme::Inspector probing = cme::Inspector::open(uri, coherency);
        return probing.readHeader().has_value();
    }
    catch (const cme::BackendError&)
    {
        // No object under that name yet. format creates it for shm: and file:.
        return false;
    }
}

[[nodiscard]] std::vector<std::string> splitOnCommas(const std::string& listed)
{
    std::vector<std::string> names;
    std::string::size_type from = 0;
    while (from <= listed.size() && !listed.empty())
    {
        const std::string::size_type comma = listed.find(',', from);
        const std::string name = listed.substr(from, comma - from);
        if (!name.empty())
        {
            names.push_back(name);
        }
        if (comma == std::string::npos)
        {
            break;
        }
        from = comma + 1;
    }
    return names;
}

void reportUsage()
{
    std::fprintf(stderr,
                 "usage: cme-format [--config <path>] [--uri <uri>] [--max-domains N]\n"
                 "                  [--max-peers N] [--strategy order|request|request_agg|peterson]\n"
                 "                  [--domains a,b,c] [--force]\n"
                 "\n"
                 "  --uri          dax:<path>[@offset], shm:/<name>, or file:<path>\n"
                 "  --config       a file to read region.uri, region.max_domains,\n"
                 "                 region.max_peers, region.strategy and region.domains from\n"
                 "  --force        format even though the region already answers\n");
}

}  // namespace

int main(int argc, char** argv)
{
    // Everything inside, because a malformed config throws out of the load below and main is the
    // one frame with nothing above it to catch.
    try
    {
        cliargs::takeArgs(argc, argv);

        // Absent path: an empty config, so every value below falls through to its own default.
        const auto deployed = kvconfig::KeyValueConfig::loadIfPresent(cliargs::get("--config", std::string{}));
        const auto uri = cliargs::get("--uri", deployed.getString("region.uri"));
        if (uri.empty())
        {
            reportUsage();
            return 2;
        }

        const auto coherency = coherencyFromName(deployed.getString("region.coherency", "cache_coherent"));
        if (!cliargs::argFlag("--force") && alreadyLive(uri, coherency))
        {
            std::fprintf(stderr,
                         "cme-format: %s already answers. Formatting it would discard the domains "
                         "and peer slots its nodes are using. Pass --force to do that anyway.\n",
                         uri.c_str());
            return 3;
        }

        cme::Session::FormatOpts_t opts;
        opts.maxDomains =
            cliargs::get("--max-domains", deployed.get("region.max_domains", opts.maxDomains));
        opts.maxPeers = cliargs::get("--max-peers", deployed.get("region.max_peers", opts.maxPeers));
        opts.strategy = strategyFromName(
            cliargs::get("--strategy", deployed.getString("region.strategy", "peterson")));
        cme::Session::format(uri, opts);

        std::printf("formatted %s: %u domain slots, %u peer slots\n", uri.c_str(), opts.maxDomains,
                    opts.maxPeers);

        const auto listed = cliargs::get("--domains", std::string{});
        const auto domains = listed.empty() ? deployed.getList("region.domains") : splitOnCommas(listed);
        if (!domains.empty())
        {
            auto session = cme::Session::open(uri);
            for (const auto& name : domains)
            {
                session.createDomain(name);
                std::printf("created domain %s\n", name.c_str());
            }
        }
    }
    catch (const std::exception& failure)
    {
        std::fprintf(stderr, "cme-format: %s\n", failure.what());
        return 1;
    }

    return 0;
}
