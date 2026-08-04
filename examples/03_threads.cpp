// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// 03_threads.cpp -- many threads of one process, over one peer slot.
//
// CME's ownership token is per peer, and a process is one peer. So Session::lock does
// not exclude this process's own threads: a second thread finds the domain already
// resident and walks straight into the critical section. It does not throw and it does
// not time out, which makes it the kind of mistake that surfaces as corrupt data much
// later.
//
// SharedSession is the fix. It adds the intra-node tier the peer model leaves out, a
// per-domain mutex over the inter-node ownership, and cohorts local waiters so the
// domain is not handed back and forth across the fabric for every thread.
//
// Use SharedSession whenever more than one thread of a process locks the same domain.

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "cme/cme.hpp"

namespace
{
constexpr int ThreadCount = 8;
constexpr int IterCount = 500;
constexpr char DomainName[] = "ledger";
}  // namespace

int main(int argc, char** argv)
{
    const std::string uri = (argc > 1) ? argv[1] : "shm:/cme-example-threads";

    cme::Session::FormatOpts_t opts;
    opts.maxPeers = 4;
    opts.maxDomains = 4;
    cme::Session::format(uri, opts);

    // One peer slot for this whole process, shared by every thread below.
    auto shared = cme::SharedSession::open(uri);
    shared.createDomain(DomainName);
    shared.joinDomain(DomainName);

    // Plain process memory. One process here, so the threads already share it; the
    // question this example answers is only who may write it at a time.
    std::int64_t counter = 0;

    std::vector<std::thread> workers;
    workers.reserve(ThreadCount);
    for (std::int32_t index = 0; index < ThreadCount; ++index)
    {
        workers.emplace_back(
            [&]
            {
                for (std::int32_t round = 0; round < IterCount; ++round)
                {
                    auto guard = shared.lock(DomainName);
                    const std::int64_t seen = counter;
                    counter = seen + 1;
                }
            });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    const std::int64_t expected = static_cast<std::int64_t>(ThreadCount) * IterCount;
    std::printf("counter = %" PRId64 ", expected %" PRId64 "\n", counter, expected);
    return (counter == expected) ? 0 : 1;
}
