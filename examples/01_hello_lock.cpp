// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// 01_hello_lock.cpp -- the smallest program that takes a CME lock.
//
// One process, POSIX shm, no hardware. Pass a URI to run it somewhere else.

#include <cstdio>
#include <string>

#include "cme/cme.hpp"

int main(int argc, char** argv)
{
    const std::string uri = (argc > 1) ? argv[1] : "shm:/cme-example-hello";

    // Once per region, by whoever creates it. format() zeroes the whole region, so a
    // process joining one that is already live must call open() alone and never this.
    // This program owns its region, and every run starts it over.
    cme::Session::FormatOpts_t opts;
    opts.maxPeers = 4;
    opts.maxDomains = 4;
    opts.strategy = cme::Strategy::Request;
    cme::Session::format(uri, opts);

    // Once per process. The Session is this process's peer slot in the region.
    auto session = cme::Session::open(uri);

    session.createDomain("inventory");

    // Domains are discoverable by name. What each one protects is a convention between
    // callers, so the region hands back names and nothing else.
    for (const auto& name : session.getDomainNames())
    {
        std::printf("domain: %s\n", name.c_str());
    }

    // Participation is opt-in: lock() without this throws NotParticipatingError.
    session.joinDomain("inventory");

    {
        auto guard = session.lock("inventory");
        std::printf("holding 'inventory'\n");
        // The critical section goes here, over data this program owns. CME arbitrates
        // the name and flushes its own metadata; your bytes are yours to publish.
    }  // Guard dtor releases

    std::printf("released\n");
    return 0;
}
