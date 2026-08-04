// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// 02_multi_process.cpp -- four processes increment one counter under one CME lock.
//
// This is the point of the library, in the smallest form that shows it. A peer is a
// process, not a thread, and each child claims its own peer slot. CME owns no user
// data: the counter lives in a mapping this program makes itself, and CME only decides
// which process may touch it. Drop the lock and the final total comes out short.

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

#include "cme/cme.hpp"

namespace
{
constexpr std::int32_t PeerCount = 4;
constexpr std::int32_t IterCount = 200;
constexpr char DomainName[] = "counter";
}  // namespace

int main(int argc, char** argv)
{
    const std::string uri = (argc > 1) ? argv[1] : "shm:/cme-example-counter";

    // The data CME protects. Anonymous shared pages, so every child sees one counter.
    void* map = mmap(nullptr, sizeof(std::int64_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED)
    {
        std::perror("mmap");
        return 1;
    }
    auto* counter = static_cast<std::int64_t*>(map);
    *counter = 0;

    cme::Session::FormatOpts_t opts;
    opts.maxPeers = PeerCount + 1;
    opts.maxDomains = 4;
    cme::Session::format(uri, opts);

    // Create the domain and join it before forking. A data domain no peer participates
    // in is an orphan, and the poll thread's sweep reclaims it, so the creator has to
    // join and stay for the children to find the name still there.
    auto owner = cme::Session::open(uri);
    owner.createDomain(DomainName);
    owner.joinDomain(DomainName);

    for (std::int32_t index = 0; index < PeerCount; ++index)
    {
        const pid_t pid = fork();
        if (pid < 0)
        {
            std::perror("fork");
            return 1;
        }
        if (pid == 0)
        {
            // fork copies `owner`, but that object owns the parent's peer slot. The child
            // never touches it and leaves through _exit, which runs no destructor.
            auto session = cme::Session::open(uri);  // this child's own peer slot
            session.joinDomain(DomainName);
            for (std::int32_t round = 0; round < IterCount; ++round)
            {
                auto guard = session.lock(DomainName);
                const std::int64_t seen = *counter;
                *counter = seen + 1;
                // Publish this program's own bytes before the Guard releases. On shm the
                // peers are cache-coherent and this only fences, but writing it here is
                // what makes the same code correct on a devdax or uncacheable region.
                cme::flush(counter, sizeof(*counter), cme::CoherencyMode::CacheCoherent);
            }
            _exit(0);
        }
    }

    bool childFailed = false;
    for (std::int32_t index = 0; index < PeerCount; ++index)
    {
        std::int32_t status = 0;
        if (wait(&status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            childFailed = true;
        }
    }

    const std::int64_t expected = static_cast<std::int64_t>(PeerCount) * IterCount;
    std::printf("counter = %" PRId64 ", expected %" PRId64 "\n", *counter, expected);
    if (childFailed)
    {
        std::printf("a child exited abnormally\n");
        return 1;
    }
    return (*counter == expected) ? 0 : 1;
}
