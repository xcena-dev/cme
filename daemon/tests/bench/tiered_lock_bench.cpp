// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// tiered_lock_bench.cpp -- what a requester pays for a critical section when a daemon holds the CXL
// turn for it. Same grid and columns as the libcme tiered bench, so a cell sits beside that one and
// what differs is the daemon in the path. One axis reads differently there: a libcme peer is a process,
// while a node here is one peer however many requesters it has, so --peers counts nodes and --threads
// counts requesters on each; cells compare at equal products.
//
// Flags, defaults in brackets:
//   --peers N      [2]     nodes, each with its own daemon
//   --workers N    [0]     workers.count for every daemon; 0 keeps the daemon's own default
//   --procs N      [1]     client processes per node, each with its own session
//   --threads N    [4]     requester threads in each of those processes
//   --domains N    [4]     data domains, each locked once per sweep
//   --iters N      [200]   sweeps per thread
//   --shuffle 0|1  [1]     visit the domains in a different order per sweep
//   --hold-us N    [0]     time spent inside each critical section
//   --strategy S   [request] request | request_agg | order | peterson
//   --hold-ms N    [5]     cohort.hold_ms for every daemon: how long one run keeps the turn
//   --uri URI      [shm:/cmed-tiered-bench]
//   --coherency S  [cache_coherent]
//   --csv PATH     [-]     append one row: peers,threads,domains,mean_us,p50_us,p90_us,p99_us,samples

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cme/shared.hpp"
#include "cmed/config.hpp"
#include "cmed/errors.hpp"
#include "cmed/guard.hpp"
#include "cmed/session.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "shared/protocol/socket_path.hpp"

namespace
{

constexpr const char* SocketDir = ".";

// A requester's own deadline. Generous, because a cell at high contention has every other requester
// ahead of it and a timeout here would be reported as a latency rather than as a refusal.
constexpr timing::Millis LockWait{60000};
constexpr timing::Millis ReachWait{10000};

// The gap between looks while waiting for a daemon to answer at all.
constexpr timing::Millis StartPoll{20};

struct Config_t
{
    std::uint32_t nodes{2};
    std::uint32_t procsPerNode{1};
    std::uint32_t threadsPerNode{4};

    // Written into every daemon's config. Zero leaves the daemon's own default, which is one per online
    // cpu up to a cap, and a run that wants this as an axis names it instead.
    std::uint32_t workers{0};
    std::uint32_t domains{4};
    std::uint32_t iters{200};
    std::uint32_t holdMicros{0};

    // Written into both configs, because a daemon worker and a requester wait on words of their own and
    // a cell that spun one of them differently would not name which one it measured.
    std::uint32_t spinMicros{1000};

    // The dispatcher's spin, which a cell varies on its own: one thread spinning costs one core, so it
    // can be held hot where the pool cannot.
    std::uint32_t dispatchSpinMicros{200};

    // Low by default, so a run gives the turn back after about one grant and a cell measures the
    // daemon in the path rather than one node's hold starving the others.
    std::uint32_t cohortHoldMillis{5};
    bool shuffle{true};
    std::string strategy{"request"};
    std::string uri{"shm:/cmed-tiered-bench"};
    std::string coherency{"cache_coherent"};
    std::string csv;
};

[[nodiscard]] std::uint32_t asCount(const char* text, std::uint32_t fallback)
{
    const std::uint64_t parsed = std::strtoul(text, nullptr, 10);
    return parsed == 0 ? fallback : static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] Config_t configFrom(int argc, char** argv)
{
    Config_t chosen;
    for (int index = 1; index + 1 < argc; index += 2)
    {
        const std::string_view flag{argv[index]};
        const char* value = argv[index + 1];

        if (flag == "--peers")
        {
            chosen.nodes = asCount(value, chosen.nodes);
        }
        else if (flag == "--procs")
        {
            chosen.procsPerNode = asCount(value, chosen.procsPerNode);
        }
        else if (flag == "--workers")
        {
            chosen.workers = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        }
        else if (flag == "--threads")
        {
            chosen.threadsPerNode = asCount(value, chosen.threadsPerNode);
        }
        else if (flag == "--domains")
        {
            chosen.domains = asCount(value, chosen.domains);
        }
        else if (flag == "--iters")
        {
            chosen.iters = asCount(value, chosen.iters);
        }
        else if (flag == "--spin-us")
        {
            chosen.spinMicros = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        }
        else if (flag == "--dispatch-spin-us")
        {
            chosen.dispatchSpinMicros = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        }
        else if (flag == "--hold-us")
        {
            chosen.holdMicros = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        }
        else if (flag == "--shuffle")
        {
            chosen.shuffle = std::strtoul(value, nullptr, 10) != 0;
        }
        else if (flag == "--hold-ms")
        {
            chosen.cohortHoldMillis = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        }
        else if (flag == "--strategy")
        {
            chosen.strategy = value;
        }
        else if (flag == "--uri")
        {
            chosen.uri = value;
        }
        else if (flag == "--coherency")
        {
            chosen.coherency = value;
        }
        else if (flag == "--csv")
        {
            chosen.csv = value;
        }
    }
    return chosen;
}

// The region's successor policy, which is a format-time choice and so the bench's rather than the
// daemon's. An unknown name is refused here rather than silently measuring the default.
[[nodiscard]] cme::Strategy strategyFrom(const std::string& name)
{
    if (name == "request")
    {
        return cme::Strategy::Request;
    }
    if (name == "request_agg")
    {
        return cme::Strategy::RequestAgg;
    }
    if (name == "order")
    {
        return cme::Strategy::Order;
    }
    if (name == "peterson")
    {
        return cme::Strategy::Peterson;
    }
    throw cmed::CmedInvalidArgumentError{"bench: unknown strategy " + name};
}

// Formatted here rather than through the region fixture, because the strategy is one of the axes. The
// session is held for the run, since an unheld region would have its domains reclaimed under the daemons.
[[nodiscard]] cme::Session formatRegion(const Config_t& chosen)
{
    cme::Session::FormatOpts_t shape;
    shape.maxDomains = chosen.domains + 1;
    shape.maxPeers = chosen.nodes + 1;
    shape.strategy = strategyFrom(chosen.strategy);
    cme::Session::format(chosen.uri, shape);

    return cme::Session::open(chosen.uri);
}

[[nodiscard]] std::string domainName(std::uint32_t domainId)
{
    return "bench" + std::to_string(domainId);
}

[[nodiscard]] std::string makeAreaName(std::uint32_t node)
{
    return "cmed-bench-" + std::to_string(node);
}

[[nodiscard]] std::string makeConfigPath(std::uint32_t node)
{
    return makeAreaName(node) + ".yaml";
}

[[nodiscard]] std::string makeSocketPath(std::uint32_t node)
{
    return cmed::protocol::buildSocketPath(SocketDir, makeAreaName(node));
}

// The chmod is not tidiness. ofstream creates through the umask, and the daemon refuses a config file
// anyone but its owner can write.
void writeConfig(std::uint32_t node, const Config_t& chosen)
{
    const std::string path = makeConfigPath(node);
    {
        std::ofstream file{path};
        file << "socket:\n"
             << "  dir: " << SocketDir << "\n"
             << "  mode: \"0600\"\n"
             << "area:\n"
             << "  name: " << makeAreaName(node) << "\n"
             << "region:\n"
             << "  uri: " << chosen.uri << "\n"
             << "  coherency: " << chosen.coherency << "\n"
             << "cohort:\n"
             << "  hold_ms: " << chosen.cohortHoldMillis << "\n"
             << "workers:\n";
        // A count of 0 is refused rather than read as "however many you would pick", so the line
        // goes in only when one was asked for.
        if (chosen.workers != 0)
        {
            file << "  count: " << chosen.workers << "\n";
        }
        file << "  spin_us: " << chosen.spinMicros << "\n"
             << "serve:\n"
             << "  spin_us: " << chosen.dispatchSpinMicros << "\n"
             << "  idle_interval_ms: 20\n"
             << "registry:\n"
             << "  refresh_interval_ms: 200\n";
    }

    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)
    {
        throw cmed::CmedInvalidArgumentError{path + ": could not restrict the config file"};
    }
}

[[nodiscard]] cmed::CmedClientConfig_t clientConfigFor(std::uint32_t node, const Config_t& chosen)
{
    cmed::CmedClientConfig_t config;
    config.socketPath = makeSocketPath(node);
    config.setupTimeout = ReachWait;
    config.lockTimeout = LockWait;
    config.spin = timing::Micros{chosen.spinMicros};
    return config;
}

// One daemon process per node, stopped by the destructor.
class NodeDaemon
{
public:
    explicit NodeDaemon(const std::string& configPath)
        : running_{spawn(configPath)}
    {
    }

    NodeDaemon(const NodeDaemon&) = delete;
    NodeDaemon(NodeDaemon&&) = delete;
    NodeDaemon& operator=(const NodeDaemon&) = delete;
    NodeDaemon& operator=(NodeDaemon&&) = delete;

    ~NodeDaemon()
    {
        if (running_ <= 0)
        {
            return;
        }

        ::kill(running_, SIGTERM);  // NOLINT(misc-include-cleaner) POSIX, via <csignal>
        int status = 0;
        static_cast<void>(::waitpid(running_, &status, 0));
    }

    [[nodiscard]] bool started() const noexcept
    {
        return running_ > 0;
    }

private:
    [[nodiscard]] static ::pid_t spawn(const std::string& configPath)
    {
        const ::pid_t child = ::fork();
        if (child != 0)
        {
            return child;
        }

        static_cast<void>(
            ::execl(CMED_BINARY_PATH, CMED_BINARY_PATH, "--config", configPath.c_str(), nullptr));
        std::_Exit(127);
    }

    ::pid_t running_;
};

// Nothing is measured until every daemon answers, so a cell does not carry another cell's startup.
[[nodiscard]] bool awaitServing(std::uint32_t node, const Config_t& chosen)
{
    return poll::waitUntil(
        [node, &chosen]
        {
            try
            {
                static_cast<void>(cmed::CmedSession::connect(clientConfigFor(node, chosen)));
                return true;
            }
            catch (const cmed::CmedError&)
            {
                return false;
            }
        },
        ReachWait, StartPoll);
}

// The same definition the libcme bench uses, so the two tables mean the same thing by p99.
[[nodiscard]] double percentile(const std::vector<std::uint64_t>& sorted, double fraction)
{
    if (sorted.empty())
    {
        return 0.0;
    }
    const auto index = static_cast<std::uint64_t>(fraction * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[index]);
}

// A different order per sweep, so a cell does not measure one fixed convoy. The generator is the
// libcme bench's, for the same reason the percentile is.
void shuffleVisitOrder(std::vector<std::uint32_t>& visit, std::uint64_t& state)
{
    for (auto index = static_cast<std::uint32_t>(visit.size()); index > 1; --index)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        std::swap(visit[index - 1], visit[state % index]);
    }
}

struct Run_t
{
    std::vector<std::uint64_t> acquireNanos;
    std::vector<std::uint64_t> releaseNanos;
    std::mutex recording;
    std::atomic<std::uint32_t> failures{0};
};

// One thread's whole run: @iters sweeps, each locking every data domain once. The stopwatch stops when
// the guard is in hand, so what is measured is the acquire and not the section.
void runSweeps(const Config_t& chosen, cmed::CmedSession& session, std::uint32_t slot, Run_t& shared)
{
    std::vector<std::uint32_t> visit(chosen.domains);
    std::iota(visit.begin(), visit.end(), 1U);
    std::uint64_t state = (static_cast<std::uint64_t>(slot) + 1) * 0x9e3779b97f4a7c15ULL;

    std::vector<std::uint64_t> mineAcquired;
    mineAcquired.reserve(static_cast<std::size_t>(chosen.iters) * chosen.domains);

    // The release is the other half of a critical section and costs its own exchange, so a run that
    // timed only the acquire would report half of what a caller pays.
    std::vector<std::uint64_t> mineReleased;
    mineReleased.reserve(static_cast<std::size_t>(chosen.iters) * chosen.domains);

    for (std::uint32_t sweep = 0; sweep < chosen.iters; ++sweep)
    {
        if (chosen.shuffle)
        {
            shuffleVisitOrder(visit, state);
        }

        for (const std::uint32_t domainId : visit)
        {
            try
            {
                const timing::Stopwatch acquiring;
                cmed::CmedGuard guard = session.lock(domainName(domainId));
                const auto acquired = static_cast<std::uint64_t>(acquiring.elapsed<timing::Nanos>().count());
                mineAcquired.push_back(acquired);

                if (chosen.holdMicros != 0)
                {
                    std::this_thread::sleep_for(timing::Micros{chosen.holdMicros});
                }

                // Explicit rather than left to the scope, so the span is the release and not whatever
                // else the destructor of a wider scope would have swept up with it.
                const timing::Stopwatch releasing;
                guard.unlock();
                mineReleased.push_back(
                    static_cast<std::uint64_t>(releasing.elapsed<timing::Nanos>().count()));
            }
            catch (const cmed::CmedError&)
            {
                shared.failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    const std::lock_guard<std::mutex> holding{shared.recording};
    shared.acquireNanos.insert(shared.acquireNanos.end(), mineAcquired.begin(), mineAcquired.end());
    shared.releaseNanos.insert(shared.releaseNanos.end(), mineReleased.begin(), mineReleased.end());
}

void removeLeftovers(const Config_t& chosen)
{
    for (std::uint32_t node = 0; node < chosen.nodes; ++node)
    {
        static_cast<void>(::unlink(makeConfigPath(node).c_str()));
        static_cast<void>(::unlink(makeSocketPath(node).c_str()));
        static_cast<void>(::unlink((makeAreaName(node) + ".lock").c_str()));
    }
}

// A shm object keeps the size it was created with, so a run asking for more peer slots than the last
// one would format into a mapping too small for them.
void removeRegion(const Config_t& chosen)
{
    constexpr std::string_view ShmScheme{"shm:"};
    if (chosen.uri.compare(0, ShmScheme.size(), ShmScheme) == 0)
    {
        static_cast<void>(::shm_unlink(chosen.uri.c_str() + ShmScheme.size()));
    }
}

[[nodiscard]] double computeMean(const std::vector<std::uint64_t>& taken)
{
    if (taken.empty())
    {
        return 0.0;
    }
    const auto sum = static_cast<double>(std::accumulate(taken.begin(), taken.end(), std::uint64_t{0}));
    return sum / static_cast<double>(taken.size());
}

// Sorts in place, because every percentile below needs it and a copy per line would be the larger cost.
// Returns the count so a caller that also wants it does not ask twice.
std::size_t describe(const char* label, std::vector<std::uint64_t>& taken)
{
    std::sort(taken.begin(), taken.end());
    std::printf("%-20s: n=%zu mean=%.0f p50=%.0f p90=%.0f p99=%.0f max=%.0f (ns)\n", label, taken.size(),
                computeMean(taken), percentile(taken, 0.50), percentile(taken, 0.90), percentile(taken, 0.99),
                percentile(taken, 1.0));
    return taken.size();
}

void report(const Config_t& chosen, Run_t& shared)
{
    std::vector<std::uint64_t>& everyAcquire = shared.acquireNanos;

    const std::size_t samples = describe("acquire", everyAcquire);
    const double mean = computeMean(everyAcquire);

    static_cast<void>(describe("release", shared.releaseNanos));
    std::printf("failures            : %u\n", shared.failures.load(std::memory_order_relaxed));

    if (chosen.csv.empty())
    {
        return;
    }

    // Appended, because the sweep writes the header once and then collects a row per cell.
    if (std::FILE* out = std::fopen(chosen.csv.c_str(), "a"))
    {
        std::fprintf(out, "%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%zu\n", chosen.nodes, chosen.threadsPerNode,
                     chosen.domains, mean / 1000.0, percentile(everyAcquire, 0.50) / 1000.0,
                     percentile(everyAcquire, 0.90) / 1000.0,
                     percentile(everyAcquire, 0.99) / 1000.0, samples);
        static_cast<void>(std::fclose(out));
    }
}

// Joins one node to one domain, waiting out the refusals that say the name has not reached that node
// yet. False once the wait is out, and the caller is the one that cleans up.
[[nodiscard]] bool joinWhenItArrives(cmed::CmedSession& asking, const std::string& name, std::uint32_t node)
{
    // The name reaches another node through that node's own registry refresh, so a refusal is a
    // reason to ask again rather than an answer.
    const bool joined = poll::waitUntil(
        [&asking, &name]
        {
            try
            {
                asking.joinDomain(name);
                return true;
            }
            catch (const cmed::CmedControlRefusedError&)
            {
                return false;
            }
        },
        ReachWait, StartPoll);

    if (!joined)
    {
        std::printf("bench: node %u never saw %s\n", node, name.c_str());
    }
    return joined;
}

}  // namespace

int main(int argc, char** argv)
{
    const Config_t chosen = configFrom(argc, argv);

    try
    {
        removeRegion(chosen);

        const cme::Session region = formatRegion(chosen);

        std::vector<std::unique_ptr<NodeDaemon>> serving;
        serving.reserve(chosen.nodes);
        for (std::uint32_t node = 0; node < chosen.nodes; ++node)
        {
            writeConfig(node, chosen);
            serving.push_back(std::make_unique<NodeDaemon>(makeConfigPath(node)));
            if (!serving.back()->started() || !awaitServing(node, chosen))
            {
                std::printf("bench: node %u never answered\n", node);
                removeLeftovers(chosen);
                return 1;
            }
        }

        // Created through node 0 and joined on every node, so no acquire pays for a join and every node
        // stays a participant for the whole run.
        std::vector<cmed::CmedSession> asking;
        asking.reserve(chosen.nodes);
        for (std::uint32_t node = 0; node < chosen.nodes; ++node)
        {
            asking.push_back(cmed::CmedSession::connect(clientConfigFor(node, chosen)));
        }

        for (std::uint32_t domainId = 1; domainId <= chosen.domains; ++domainId)
        {
            asking[0].createDomain(domainName(domainId));
        }
        for (std::uint32_t node = 0; node < chosen.nodes; ++node)
        {
            for (std::uint32_t domainId = 1; domainId <= chosen.domains; ++domainId)
            {
                if (!joinWhenItArrives(asking[node], domainName(domainId), node))
                {
                    removeLeftovers(chosen);
                    return 1;
                }
            }
        }

        Run_t shared;
        shared.acquireNanos.reserve(static_cast<std::size_t>(chosen.nodes) * chosen.threadsPerNode *
                                    chosen.iters * chosen.domains);

        std::vector<std::thread> running;
        running.reserve(static_cast<std::size_t>(chosen.nodes) * chosen.threadsPerNode);

        const timing::Stopwatch wall;
        for (std::uint32_t slot = 0; slot < chosen.nodes * chosen.threadsPerNode; ++slot)
        {
            const std::uint32_t node = slot / chosen.threadsPerNode;
            running.emplace_back(
                [&chosen, &asking, &shared, slot, node]
                {
                    runSweeps(chosen, asking[node], slot, shared);
                });
        }
        for (std::thread& thread : running)
        {
            thread.join();
        }

        std::printf(
            "cmed tiered     : %s, %u node(s) x %u thread(s), %u domain(s), %u iters, hold=%ums"
            " in %lldms\n",
            chosen.strategy.c_str(), chosen.nodes, chosen.threadsPerNode, chosen.domains,
            chosen.iters, chosen.cohortHoldMillis,
            static_cast<long long>(wall.elapsed<timing::Millis>().count()));
        report(chosen, shared);

        removeLeftovers(chosen);
        return shared.failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
    }
    catch (const std::exception& failure)
    {
        std::printf("bench threw: %s\n", failure.what());
        removeLeftovers(chosen);
        return 1;
    }
}
