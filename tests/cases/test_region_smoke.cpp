// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// region_smoke_test.cpp -- smoke test for ShmMemory + formatRegion +
// Memory::header(). No Peer / Strategy involvement yet.

#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "core/algo/peer.hpp"
#include "core/types.hpp"
#include "observe/inspector.hpp"
#include "test_context.hpp"

namespace
{

// Formats the region and confirms header() reflects what create() wrote.
cme::Geometry::Header_t checkCreatorFormat(harness::TestContext& ctx, cme::Geometry& region,
                                           cme::DomainId numDomains,
                                           cme::PeerId maxPeers)
{
    const std::uint64_t expectedSize =
        cme::Geometry::computeAreaSize(numDomains, maxPeers, cme::Strategy::Order, 0);
    auto* const header = region.getHeader();
    std::printf("creator formatted region (base=%p size=%zu)\n",
                static_cast<void*>(header), static_cast<std::size_t>(expectedSize));

    ctx.check(header != nullptr, "base() non-null");
    ctx.check(expectedSize >= 64, "size() at least one cacheline");

    const auto hdr = *header;
    ctx.check(static_cast<cme::Strategy>(static_cast<std::uint32_t>(hdr.strategy)) == cme::Strategy::Order, "strategy round-trips");
    ctx.check(hdr.numDomains == numDomains, "numDomains round-trips");
    ctx.check(hdr.maxPeers == maxPeers, "maxPeers round-trips");
    ctx.check(hdr.totalSize == expectedSize, "totalSize matches mapping");
    ctx.check(hdr.formatGeneration != 0, "formatGeneration non-zero");
    return hdr;
}

// A second opener attaches to the live region (same name) and reads the same header. Its
// dtor unmaps but does NOT unlink; the creator's dtor does.
void checkJoinerSeesSameHeader(const std::string& shmUri, const cme::Geometry::Header_t& hdr,
                               harness::TestContext& ctx)
{
    auto joiner = cme::Geometry::open(shmUri);
    joiner.bindBlocking(std::chrono::milliseconds{0}, ctx.coherency());
    const auto joinerHeader = *joiner.getHeader();
    ctx.check(joinerHeader.formatGeneration == hdr.formatGeneration,
              "joiner sees same formatGeneration");
    ctx.check(joinerHeader.numDomains == hdr.numDomains, "joiner sees same numDomains");
    ctx.check(joinerHeader.maxPeers == hdr.maxPeers, "joiner sees same maxPeers");
}

// Read-only Inspector view (via /dev/shm) matches the creator's header, ownership, and peer
// state; out-of-range / sentinel queries report nullopt instead of throwing.
void checkInspectorMatchesHeader(const std::string& shmUri, const cme::Geometry::Header_t& hdr,
                                 cme::DomainId numDomains, cme::PeerId maxPeers,
                                 harness::TestContext& ctx)
{
    const cme::Inspector inspector = cme::Inspector::open(shmUri, ctx.coherency());
    const auto headerInfo = inspector.readHeader();
    ctx.check(headerInfo.has_value(), "inspector readHeader() returns value");
    if (headerInfo)
    {
        ctx.check(static_cast<std::uint32_t>(headerInfo->strategy) == static_cast<std::uint32_t>(hdr.strategy), "inspector strategy matches creator");
        ctx.check(headerInfo->numDomains == hdr.numDomains, "inspector numDomains matches");
        ctx.check(headerInfo->maxPeers == hdr.maxPeers, "inspector maxPeers matches");
        ctx.check(headerInfo->formatGeneration == hdr.formatGeneration,
                  "inspector formatGeneration matches");
    }

    const auto ownership0 = inspector.readOwnership(0);
    ctx.check(ownership0.has_value(), "inspector readOwnership(0) returns value");
    if (ownership0)
    {
        // Genesis owner = peer 0 (no vacant/NoPeer runtime state).
        ctx.check(ownership0->holder == 0, "post-format ownership(0).holder == peer 0");
        ctx.check(ownership0->epoch == 0, "post-format ownership(0).epoch == 0");
    }

    const auto peerInfo0 = inspector.readPeer(0);
    ctx.check(peerInfo0.has_value(), "inspector readPeer(0) returns value");
    if (peerInfo0)
    {
        ctx.check(!peerInfo0->active, "post-format peer(0) inactive (no join yet)");
        ctx.check(peerInfo0->selfId == 0, "inspector peer(0).selfId == 0");
        ctx.check(peerInfo0->time.poll == 0, "no poll CPU before peer joins");
        ctx.check(peerInfo0->time.worker == 0, "no worker CPU before peer joins");
    }

    // out-of-range queries should report nullopt, not throw.
    ctx.check(!inspector.readOwnership(numDomains).has_value(),
              "inspector readOwnership() out-of-range -> nullopt");
    ctx.check(!inspector.readPeer(maxPeers).has_value(),
              "inspector readPeer() out-of-range -> nullopt");
    ctx.check(!inspector.readPeer(cme::NoPeer).has_value(),
              "inspector readPeer(NoPeer) sentinel -> nullopt");
}

// Peer create + acquire/release cycles. Runs AFTER the Inspector check so the inspector sees
// the pristine post-format state, not a half-acquired one.
void checkPeerAcquireReleaseCycles(cme::Geometry& region, cme::DomainId numDomains,
                                   harness::TestContext& ctx)
{
    cme::Peer peer{region, 0, ctx.coherency()};
    ctx.check(peer.getPeerId() == 0, "Peer::getPeerId() matches ctor arg");

    // Create + join the data domains (slots 1..numDomains-1); slot 0 is
    // control (joined by default). Opt-in: createDomain alone does not
    // participate, so join before locking.
    for (cme::DomainId domainId = 1; domainId < numDomains; ++domainId)
    {
        peer.joinDomain(peer.createDomain("lane" + std::to_string(domainId)));
    }

    // Single-peer ORDER strategy: peer 0 holds control (genesis) +
    // every domain it created (no contention); acquire/release should
    // round-trip without ever hitting the 5 s deadline.
    for (int i = 0; i < 100; ++i)
    {
        auto guard = peer.lock(0);
    }
    ctx.check(true, "100 acquire/release cycles on domain 0 (no timeout)");

    for (cme::DomainId domainId = 0; domainId < numDomains; ++domainId)
    {
        auto guard = peer.lock(domainId);
    }
    ctx.check(true, "acquire/release each domain once");
}

// ORDER strategy: create -> joiner attach -> inspector read -> Peer acquire/release, all on
// one region.
void checkOrderStrategyRoundTrip(const std::string& shmUri, cme::DomainId numDomains,
                                 cme::PeerId maxPeers, harness::TestContext& ctx)
{
    auto region = cme::Geometry::create(shmUri, numDomains, maxPeers,
                                        cme::Geometry::FormatOpts_t{cme::Strategy::Order});
    const auto hdr = checkCreatorFormat(ctx, region, numDomains, maxPeers);
    checkJoinerSeesSameHeader(shmUri, hdr, ctx);
    checkInspectorMatchesHeader(shmUri, hdr, numDomains, maxPeers, ctx);
    checkPeerAcquireReleaseCycles(region, numDomains, ctx);
    ctx.check(true, "Peer dtor returned without crash");
}

// REQUEST strategy single-peer round-trip, on its own region so it does not reuse the
// ORDER-strategy header. Exercises the doorbell hand-raise + grant path.
void checkRequestStrategyRoundTrip(cme::DomainId numDomains, cme::PeerId maxPeers,
                                   harness::TestContext& ctx)
{
    constexpr const char* ReqName = "/cme_region_smoke_req";
    const std::string reqUri = std::string{"shm:"} + ReqName;
    auto region = cme::Geometry::create(reqUri, numDomains, maxPeers,
                                        cme::Geometry::FormatOpts_t{cme::Strategy::Request});
    cme::Peer peer{region, 0, ctx.coherency()};
    for (cme::DomainId domainId = 1; domainId < numDomains; ++domainId)
    {
        peer.joinDomain(peer.createDomain("lane" + std::to_string(domainId)));
    }
    for (int i = 0; i < 100; ++i)
    {
        auto guard = peer.lock(0);
    }
    ctx.check(true, "REQUEST: 100 acquire/release cycles on domain 0");
    for (cme::DomainId domainId = 0; domainId < numDomains; ++domainId)
    {
        auto guard = peer.lock(domainId);
    }
    ctx.check(true, "REQUEST: acquire/release each domain once");
}

// Memory backends do not auto-unlink on dtor, so after an explicit shm_unlink, a fresh joiner
// must fail (ENOENT), which surfaces as FormatError.
void checkUnlinkedJoinerFails(harness::TestContext& ctx, const char* name,
                              const std::string& shmUri)
{
    ::shm_unlink(name);
    bool freshJoinerThrows = false;
    try
    {
        auto stale = cme::Geometry::open(shmUri);
        (void)stale;
    }
    catch (const cme::FormatError&)
    {
        freshJoinerThrows = true;
    }
    ctx.check(freshJoinerThrows, "joiner on unlinked name fails");
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    constexpr const char* Name = "/cme_region_smoke";
    constexpr cme::DomainId NumDomains = 4;
    constexpr cme::PeerId MaxPeers = 8;

    const std::string shmUri = std::string{"shm:"} + Name;

    // Phase A/B/B'/B'': single ORDER-strategy region, creator -> joiner -> inspector -> Peer.
    try
    {
        checkOrderStrategyRoundTrip(shmUri, NumDomains, MaxPeers, ctx);
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: ORDER exception: %s\n", e.what());
        ctx.recordFailure();
    }

    // Phase D: REQUEST strategy. ORDER strategy was checked above.
    try
    {
        checkRequestStrategyRoundTrip(NumDomains, MaxPeers, ctx);
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: REQUEST exception: %s\n", e.what());
        ctx.recordFailure();
    }

    // Phase C: shm_unlink + stale-joiner check.
    checkUnlinkedJoinerFails(ctx, Name, shmUri);
}

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, runBody);
}
