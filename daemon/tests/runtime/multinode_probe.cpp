// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// multinode_probe.cpp -- several daemons on one region, each with a requester, testing cross-node
// exclusion: every daemon is a cme peer, so a domain one holds must exclude the others through the
// region rather than a mutex. Nodes are processes; names travel only through each daemon's own registry
// refresh, never written by hand.
//
// Registered once per medium with two nodes: --backend and --slot name a medium the site config
// resolves, --nodes raises the count, and --uri names a medium whole. The region is formatted here,
// so the uri must be one the caller may format.

#include <atomic>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "cmed/config.hpp"
#include "cmed/errors.hpp"
#include "cmed/guard.hpp"
#include "cmed/session.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "harness/helper.hpp"
#include "harness/helper_cme_region.hpp"
#include "harness/helper_medium.hpp"
#include "harness/helper_requester.hpp"
#include "harness/helper_scratch.hpp"
#include "harness/helper_served_daemon.hpp"
#include "test_memory.hpp"
#include "test_options.hpp"

namespace
{

constexpr std::uint32_t DefaultNodes = 2;

// A ceiling, not a target. The region's peer table has to hold every node plus this probe's own
// session, and a run asking for more than a plausible host has is a typo rather than a request.
constexpr std::uint32_t MostNodes = 16;

constexpr const char* DomainName = "shared0";

// Created after every daemon is already serving, so the only way a requester learns of it is the
// registry refresh each daemon runs on its own turn.
constexpr const char* LateDomainName = "shared1";

// Created by a requester through its daemon rather than by this probe's own session. Nothing writes it
// into the region here, which is what makes the control path the only way it can exist.
constexpr const char* AskedDomainName = "shared2";

// Held and released by every other node before node 0 deletes it, which is what a peer that stayed a
// participant after its turn would make impossible.
constexpr const char* HandedDomainName = "shared3";

// Joined by another node and kept, so this one names what a participant blocks and what leaving returns.
constexpr const char* KeptDomainName = "shared4";

// Created on one node and deleted from another, which is the only arrangement where a node can be outside
// a domain that exists: a create leaves its own node inside it.
constexpr const char* UnjoinedDomainName = "shared5";

// Slot zero is the control domain; the rest cover every data domain name declared above.
constexpr std::uint32_t RegionSlots = 7;

// Enough rounds that every node gets several turns, few enough that the region traffic stays inside the
// suite's budget. Each acquire may cross to another peer, which is the expensive part.
constexpr std::uint32_t Rounds = 5;

// The hold has to dominate the round trip, or the case passes on timing rather than exclusion: threads
// briefly inside rarely collide even unprotected. A/B with separate regions per daemon fails the case.
constexpr timing::Millis HoldFor{20};

// A real acquire crosses to another peer and back, so a requester's own deadline has to allow for the
// daemon's cme acquire under contention from every other node.
constexpr timing::Millis LockWait{10000};
constexpr timing::Millis ReachWait{5000};

// Far above the refresh interval these configs ask for, because what is waited on is the daemon's own
// clock and a case pinned to exactly one interval would be racing it.
constexpr timing::Millis RefreshWait{5000};

// What one run was asked for. The node count is this probe's alone, so it is read here and the rest
// comes off the shared medium flags.
struct Options_t : cmed::harness::MediumOptions_t
{
    std::uint32_t nodes{DefaultNodes};

    void parse(int argc, char** argv)
    {
        MediumOptions_t::parse(argc, argv);
        for (int index = 1; index + 1 < argc; index += 2)
        {
            if (std::string_view{argv[index]} == "--nodes")
            {
                nodes = static_cast<std::uint32_t>(std::strtoul(argv[index + 1], nullptr, 10));
            }
        }
    }
};

// The three the harness composes, with this probe's own indexing in front of them. Nodes are named by
// position here, and the shared unit is named by area, so this is where the two meet.
[[nodiscard]] std::string makeConfigPath(const cmed::harness::ProbeScratch& scratch,
                                         const std::vector<std::string>& areaNames, std::uint32_t node)
{
    return cmed::harness::daemonConfigPath(scratch, areaNames.at(node));
}

void writeConfig(const cmed::harness::ProbeScratch& scratch, const std::vector<std::string>& areaNames,
                 std::uint32_t node, const Options_t& chosen)
{
    cmed::harness::DaemonSite_t site;
    site.areaName = areaNames.at(node);
    site.uri = chosen.uri;
    site.coherency = chosen.coherency;

    // A cohort cap, because what this probe contends on is a domain several nodes want and an
    // uncapped run would let one node keep it. Both loops turn faster than a deployment's would.
    site.extra = "cohort:\n  cap: 4\nserve:\n  idle_interval_ms: 50\nregistry:\n  refresh_interval_ms: 100\n";
    cmed::harness::writeDaemonConfig(scratch, site);
}

[[nodiscard]] cmed::CmedClientConfig_t configFor(const cmed::harness::ProbeScratch& scratch,
                                                 const std::vector<std::string>& areaNames, std::uint32_t node)
{
    return cmed::harness::clientConfigFor(scratch, areaNames.at(node), ReachWait, LockWait);
}

// What every contending thread shares. The counter is plain on purpose, because a second holder is what
// corrupts it.
struct Contention_t
{
    std::atomic<std::uint32_t> inside{0};
    std::atomic<bool> overlapped{false};
    std::atomic<std::uint32_t> failures{0};
    std::uint32_t counter{0};
};

void contend(const cmed::harness::ProbeScratch& scratch, const std::vector<std::string>& areaNames,
             Contention_t& shared, std::uint32_t node)
{
    try
    {
        cmed::CmedSession session = cmed::harness::openWhenItAnswers(configFor(scratch, areaNames, node));

        for (std::uint32_t round = 0; round < Rounds; ++round)
        {
            const cmed::CmedGuard guard = session.lock(DomainName);
            if (shared.inside.fetch_add(1, std::memory_order_acq_rel) != 0)
            {
                shared.overlapped.store(true, std::memory_order_release);
            }
            ++shared.counter;
            std::this_thread::sleep_for(HoldFor);
            shared.inside.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    catch (const cmed::CmedError& failure)
    {
        std::printf("contender on node %" PRIu32 ": %s\n", node, failure.what());
        shared.failures.fetch_add(1, std::memory_order_acq_rel);
    }
}

// The name was never written by this process. It reached the requester because each daemon read the
// region and published what it found, which is the only path a name has to a requester.
void aNameFromTheRegionResolves(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                                const std::vector<std::string>& areaNames)
{
    ctx.openCase("a domain name published out of the region");

    cmed::CmedSession session = cmed::harness::openWhenItAnswers(configFor(scratch, areaNames, 0));

    {
        const cmed::CmedGuard guard = session.lock(DomainName);
        ctx.check(static_cast<bool>(guard), "the requester locks a name it never wrote");
    }

    bool refused = false;
    try
    {
        static_cast<void>(session.lock("absent"));
    }
    catch (const cmed::CmedUnknownDomainError&)
    {
        refused = true;
    }
    ctx.check(refused, "and a name the region does not carry is refused");
}

// The boundary this probe exists for. Every requester names one domain and each of their daemons is a
// peer of one region, so an overlap here is cme mutual exclusion failing between nodes.
void everyNodeTakesItsTurn(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                           const std::vector<std::string>& areaNames, std::uint32_t nodes)
{
    ctx.openCase("one domain, one requester per daemon");

    Contention_t shared;
    std::vector<std::thread> contenders;
    contenders.reserve(nodes);
    for (std::uint32_t node = 0; node < nodes; ++node)
    {
        contenders.emplace_back(
            [&scratch, &areaNames, &shared, node]
            {
                contend(scratch, areaNames, shared, node);
            });
    }
    for (std::thread& contender : contenders)
    {
        contender.join();
    }

    ctx.check(shared.failures.load(std::memory_order_acquire) == 0, "every requester ran to the end");
    ctx.check(!shared.overlapped.load(std::memory_order_acquire),
              "no instant had the domain held on two nodes");
    ctx.checkf(shared.counter == nodes * Rounds, "%" PRIu32 " critical sections ran, one per acquire",
               shared.counter);
}

// A domain created while every daemon is already serving. The refresh each of them runs on its own turn
// is the only path by which that name reaches a requester, and nothing else here would notice it stop.
void aLateDomainBecomesVisible(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                               const std::vector<std::string>& areaNames, std::uint32_t node, const char* name)
{
    cmed::CmedSession session = cmed::harness::openWhenItAnswers(configFor(scratch, areaNames, node));

    // Retried rather than slept on, because what is waited for is the daemon's own clock.
    const bool locked = poll::waitUntil(
        [&session, name]
        {
            try
            {
                const cmed::CmedGuard guard = session.lock(name);
                return static_cast<bool>(guard);
            }
            catch (const cmed::CmedUnknownDomainError&)
            {
                return false;
            }
        },
        RefreshWait, cmed::harness::ProbePoll);

    ctx.checkf(locked, "node %" PRIu32 " reaches `%s` without being restarted", node, name);
}

// The whole control path: the requester talks to its daemon, the daemon talks to cme, and the registry
// carries the name back to the requester that asked for it.
void aRequesterCreatesAndDeletes(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                                 const std::vector<std::string>& areaNames)
{
    ctx.openCase("create and delete through the daemon");

    cmed::CmedSession session = cmed::harness::openWhenItAnswers(configFor(scratch, areaNames, 0));
    session.createDomain(AskedDomainName);

    {
        const cmed::CmedGuard guard = session.lock(AskedDomainName);
        ctx.check(static_cast<bool>(guard), "the requester locks what it created");
    }

    // Idempotent: a create names the state the caller wants, which is that the domain is there and
    // this session is in it. A second one finds both already true and says so.
    bool again = true;
    try
    {
        session.createDomain(AskedDomainName);
    }
    catch (const cmed::CmedControlRefusedError&)
    {
        again = false;
    }
    ctx.check(again, "a second create takes, since the name it asks for is already there");

    session.deleteDomain(AskedDomainName);

    bool absent = false;
    try
    {
        // The cast is what the call is for: this line is expected to throw, not to return a guard.
        static_cast<void>(session.lock(AskedDomainName));
    }
    catch (const cmed::CmedUnknownDomainError&)
    {
        absent = true;
    }
    ctx.check(absent, "and the name it deleted stops resolving");

    bool missing = false;
    try
    {
        session.deleteDomain(AskedDomainName);
    }
    catch (const cmed::CmedControlRefusedError& refused)
    {
        missing = refused.code() == std::errc::no_such_file_or_directory;
    }
    ctx.check(missing, "deleting it twice says there is no such domain");
}

// A domain is deletable only while no peer participates, and a daemon participates only for as long as
// it holds the turn. So a domain another node has finished with is deletable from here.
void anotherNodesDomainCanBeDeleted(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                                    const std::vector<std::string>& areaNames, std::uint32_t nodes)
{
    ctx.openCase("deleting a domain another node has locked");

    cmed::CmedSession first = cmed::harness::openWhenItAnswers(configFor(scratch, areaNames, 0));
    first.createDomain(HandedDomainName);

    // Every other node takes the turn and gives it back, which is what makes each of them a participant
    // and then not one.
    for (std::uint32_t node = 1; node < nodes; ++node)
    {
        aLateDomainBecomesVisible(ctx, scratch, areaNames, node, HandedDomainName);
    }

    bool deleted = true;
    try
    {
        first.deleteDomain(HandedDomainName);
    }
    catch (const cmed::CmedControlRefusedError& refused)
    {
        std::printf("delete refused: %s\n", refused.what());
        deleted = false;
    }
    ctx.check(deleted, "node 0 deletes what every other node has held and released");
}

// Joining is also how a case waits for a name to arrive without taking the turn. A lock would make the
// daemon join and then leave on the hand-back, which is the participation this case is about.
[[nodiscard]] bool awaitJoinable(cmed::CmedSession& session, const char* name)
{
    return poll::waitUntil(
        [&session, name]
        {
            try
            {
                session.joinDomain(name);
                return true;
            }
            catch (const cmed::CmedControlRefusedError&)
            {
                return false;
            }
        },
        RefreshWait, cmed::harness::ProbePoll);
}

// What a join buys and what it costs. It keeps this node a participant across turns, so no other node can
// delete the domain until it leaves, and that is the same rule the hand-back obeys by leaving.
void aJoinedDomainResistsDeletion(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                                  const std::vector<std::string>& areaNames)
{
    ctx.openCase("a domain another node joined");

    cmed::CmedSession first = cmed::harness::openWhenItAnswers(configFor(scratch, areaNames, 0));
    first.createDomain(KeptDomainName);

    cmed::CmedSession second = cmed::harness::openWhenItAnswers(configFor(scratch, areaNames, 1));
    if (!ctx.check(awaitJoinable(second, KeptDomainName), "node 1 joins it"))
    {
        return;
    }

    // The code and not merely a refusal: a delete that failed for its own reasons would pass a check that
    // only asks whether something was thrown.
    bool refused = false;
    try
    {
        first.deleteDomain(KeptDomainName);
    }
    catch (const cmed::CmedControlRefusedError& blocked)
    {
        refused = blocked.code() == std::errc::directory_not_empty;
    }
    ctx.check(refused, "node 0 cannot delete it while node 1 participates");

    second.leaveDomain(KeptDomainName);

    bool deleted = true;
    try
    {
        first.deleteDomain(KeptDomainName);
    }
    catch (const cmed::CmedControlRefusedError& again)
    {
        std::printf("delete after leave refused: %s\n", again.what());
        deleted = false;
    }
    ctx.check(deleted, "and can once node 1 has left");
}

// Asks until the answer stops being "no such domain", so the case that follows is about participation and
// not about a name the registry refresh has yet to carry. Empty means the delete went through.
[[nodiscard]] std::error_code awaitDeleteAnswer(cmed::CmedSession& session, const char* name)
{
    const std::optional<std::error_code> answered = poll::awaitValue(
        [&session, name]() -> std::optional<std::error_code>
        {
            try
            {
                session.deleteDomain(name);
                return std::error_code{};
            }
            catch (const cmed::CmedControlRefusedError& refused)
            {
                if (refused.code() != std::errc::no_such_file_or_directory)
                {
                    return refused.code();
                }
                return std::nullopt;
            }
        },
        RefreshWait, cmed::harness::ProbePoll);

    // Nothing means the wait ran out on the one answer it was waiting past, which is what the
    // caller is told.
    return answered.value_or(std::make_error_code(std::errc::no_such_file_or_directory));
}

// Deleting takes the domain's turn, so a node outside the domain has no delete to make. This is the rule
// stated from the outside: the refusal, then the join that turns it into a delete.
void anUnjoinedDomainCannotBeDeleted(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch,
                                     const std::vector<std::string>& areaNames)
{
    ctx.openCase("deleting a domain this node never joined");

    cmed::CmedSession second = cmed::harness::openWhenItAnswers(configFor(scratch, areaNames, 1));
    second.createDomain(UnjoinedDomainName);

    cmed::CmedSession first = cmed::harness::openWhenItAnswers(configFor(scratch, areaNames, 0));
    ctx.check(awaitDeleteAnswer(first, UnjoinedDomainName) == std::errc::operation_not_permitted,
              "node 0 is refused a domain no requester of its own is in");

    if (!ctx.check(awaitJoinable(first, UnjoinedDomainName), "node 0 joins it"))
    {
        return;
    }

    // Node 1 can leave only now. A sole participant has no exit but the delete, and node 0's join is what
    // gives node 1 a successor.
    second.leaveDomain(UnjoinedDomainName);

    bool deleted = true;
    try
    {
        first.deleteDomain(UnjoinedDomainName);
    }
    catch (const cmed::CmedControlRefusedError& again)
    {
        std::printf("delete after join refused: %s\n", again.what());
        deleted = false;
    }
    ctx.check(deleted, "and the same delete lands once it has");
}

}  // namespace

int main(int argc, char** argv)
{
    cmed::harness::ProbeScratch scratch{"multinode-probe"};

    Options_t chosen;
    chosen.parse(argc, argv);
    if (chosen.nodes == 0 || chosen.nodes > MostNodes)
    {
        std::printf("multinode probe: --nodes %" PRIu32 " is outside 1..%" PRIu32 "\n", chosen.nodes,
                    MostNodes);
        return 1;
    }

    // Held for the whole run, because its destructor is what takes the area away afterwards. Empty
    // when --uri named a medium the caller owns.
    std::unique_ptr<harness::TestMemory> area;
    try
    {
        area = chosen.resolve();
    }
    catch (const harness::MediumUnavailable& absent)
    {
        std::printf("SKIP: %s\n", absent.what());
        return harness::SkipExitCode;
    }

    std::vector<std::string> areaNames;
    areaNames.reserve(chosen.nodes);
    for (std::uint32_t node = 0; node < chosen.nodes; ++node)
    {
        areaNames.push_back(scratch.makeAreaName(std::to_string(node)));
    }

    bool passed = false;
    try
    {
        // The first domain exists before any daemon starts, because a daemon publishes what it finds at
        // startup and a later one waits for a refresh. One peer slot per node, plus this session's.
        cmed::harness::ProbeRegion region{chosen.uri, RegionSlots, chosen.nodes + 1,
                                          chosen.coherencyMode()};
        region.createDomain(DomainName);

        std::vector<std::unique_ptr<cmed::harness::NodeDaemon>> serving;
        serving.reserve(chosen.nodes);
        for (std::uint32_t node = 0; node < chosen.nodes; ++node)
        {
            writeConfig(scratch, areaNames, node, chosen);
            serving.push_back(std::make_unique<cmed::harness::NodeDaemon>(makeConfigPath(scratch, areaNames, node)));

            if (!serving.back()->started())
            {
                std::printf("multinode probe could not start node %" PRIu32 "\n", node);
                return 1;
            }
        }

        const std::uint32_t nodes = chosen.nodes;
        passed = probe::run(
                     "multinode probe",
                     [&region, &scratch, &areaNames, nodes](probe::Context& ctx)
                     {
                         aNameFromTheRegionResolves(ctx, scratch, areaNames);
                         everyNodeTakesItsTurn(ctx, scratch, areaNames, nodes);

                         ctx.openCase("a domain created while every daemon is serving");
                         region.createDomain(LateDomainName);
                         // A for statement sequences its condition before its increment, so the
                         // two are not one expression and there is no order to get wrong.
                         // NOLINTNEXTLINE(bugprone-inc-dec-in-conditions)
                         for (std::uint32_t node = 0; node < nodes; ++node)
                         {
                             aLateDomainBecomesVisible(ctx, scratch, areaNames, node, LateDomainName);
                         }

                         aRequesterCreatesAndDeletes(ctx, scratch, areaNames);
                         anotherNodesDomainCanBeDeleted(ctx, scratch, areaNames, nodes);

                         if (nodes > 1)
                         {
                             aJoinedDomainResistsDeletion(ctx, scratch, areaNames);
                             anUnjoinedDomainCannotBeDeleted(ctx, scratch, areaNames);
                         }
                     }) == 0;
    }
    catch (const std::exception& failure)
    {
        std::printf("multinode probe threw: %s\n", failure.what());
        passed = false;
    }

    return passed ? 0 : 1;
}
