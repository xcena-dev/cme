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

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/args.hpp"
#include "common/timing.hpp"

namespace
{

// Short, because this is asking whether anyone is there and not waiting for them to arrive.
constexpr timing::Millis AskIfLive{200};

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

// True when the region answers, so formatting would take it from whoever is on it.
[[nodiscard]] bool alreadyLive(const std::string& uri)
{
    try
    {
        cme::Session::OpenOpts_t opts;
        opts.formatTimeout = AskIfLive;
        const cme::Session probing = cme::Session::open(uri, opts);
        return true;
    }
    catch (const cme::RegionNotFormattedError&)
    {
        // Nothing has been laid out here, which is what this tool is for.
        return false;
    }
    catch (const cme::BackendError&)
    {
        // No object under that name yet. format creates it for shm: and file:.
        return false;
    }
    catch (const cme::NoFreeSlotError&)
    {
        // Formatted, and every slot taken. The busiest possible region is still a live one.
        return true;
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
                 "usage: cme-format --uri <uri> [--max-domains N] [--max-peers N]\n"
                 "                  [--strategy order|request|request_agg|peterson]\n"
                 "                  [--domains a,b,c] [--force]\n"
                 "\n"
                 "  --uri          dax:<path>[@offset], shm:/<name>, or file:<path>\n"
                 "  --force        format even though the region already answers\n");
}

}  // namespace

int main(int argc, char** argv)
{
    cliargs::takeArgs(argc, argv);

    const std::string uri = cliargs::argStr("--uri", std::string{});
    if (uri.empty())
    {
        reportUsage();
        return 2;
    }

    try
    {
        if (!cliargs::argFlag("--force") && alreadyLive(uri))
        {
            std::fprintf(stderr,
                         "cme-format: %s already answers. Formatting it would discard the domains "
                         "and peer slots its nodes are using. Pass --force to do that anyway.\n",
                         uri.c_str());
            return 3;
        }

        cme::Session::FormatOpts_t opts;
        opts.maxDomains = static_cast<std::uint32_t>(cliargs::argU64("--max-domains", opts.maxDomains));
        opts.maxPeers = static_cast<std::uint32_t>(cliargs::argU64("--max-peers", opts.maxPeers));
        opts.strategy = strategyFromName(cliargs::argStr("--strategy", "peterson"));
        cme::Session::format(uri, opts);

        std::printf("formatted %s: %u domain slots, %u peer slots\n", uri.c_str(), opts.maxDomains,
                    opts.maxPeers);

        const std::vector<std::string> domains = splitOnCommas(cliargs::argStr("--domains", std::string{}));
        if (!domains.empty())
        {
            cme::Session session = cme::Session::open(uri);
            for (const std::string& name : domains)
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
