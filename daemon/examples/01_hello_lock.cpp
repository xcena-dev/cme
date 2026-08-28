// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// 01_hello_lock.cpp -- the smallest program that takes a domain's turn through cmed.
//
// Needs a daemon already serving. Pass its socket path, or let the argument default to what
// deploy/cmed.example.yaml names. Nothing here opens the region: the daemon holds the node's one
// peer slot, and this process asks it for turns.

#include <cstdio>
#include <string>

#include "cmed/errors.hpp"
#include "cmed/guard.hpp"
#include "cmed/session.hpp"

int main(int argc, char** argv)
{
    const std::string socketPath = (argc > 1) ? argv[1] : "/run/cmed/cmed.sock";

    try
    {
        // Once per process. Everything below travels over this one connection, which the daemon also
        // watches for the hangup that says this requester died.
        cmed::CmedSession session = cmed::CmedSession::connect(socketPath);

        // Whoever gets there first creates it. A second caller's create is refused with EEXIST, which
        // is why a program that only wants to use the domain joins instead.
        try
        {
            session.createDomain("inventory");
        }
        catch (const cmed::CmedControlRefusedError& refused)
        {
            std::printf("create refused, joining instead: %s\n", refused.what());
            session.joinDomain("inventory");
        }

        {
            const cmed::CmedGuard guard = session.lock("inventory");
            std::printf("holding 'inventory'\n");

            // The critical section goes here, over data this program owns. cmed arbitrates the name
            // across the fabric; the bytes are yours to publish.
        }  // The guard's destructor gives the turn back.

        std::printf("released\n");
    }
    catch (const cmed::CmedError& failure)
    {
        std::printf("cmed: %s\n", failure.what());
        return 1;
    }

    return 0;
}
