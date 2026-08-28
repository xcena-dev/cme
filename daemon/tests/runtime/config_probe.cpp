// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// config_probe.cpp -- the settings a file is allowed to change, and the ones it is not.
//
// Two files, one reader each: the daemon's names the region and the area, an application's carries its
// own deadlines. The one key that crosses is the area name, read from the daemon's file; an application
// naming its own area must not be able to. The last cases parse the shipped examples, holding them to
// landing where they claim.
//
// Every refusal here is a deployment mistake caught at load. A refusal that stopped being one would
// start a daemon on settings nobody wrote down, so each is its own named check.

// The mode bits below have a definition chain ending in bits/, which include-cleaner is told to
// ignore, so it credits neither this header for providing them nor this file for using them.
#include <sys/stat.h>  // NOLINT(misc-include-cleaner)

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

#include "cmed/config.hpp"
#include "cmed/errors.hpp"
#include "common/kv_config.hpp"
#include "common/timing.hpp"
#include "daemon/startup/config.hpp"
#include "harness/helper_scratch.hpp"
#include "tests/probe_context.hpp"

#ifndef CMED_DAEMON_EXAMPLE_PATH
#define CMED_DAEMON_EXAMPLE_PATH ""
#endif

#ifndef CMED_CLIENT_EXAMPLE_PATH
#define CMED_CLIENT_EXAMPLE_PATH ""
#endif

#ifndef CMED_DEV_EXAMPLE_PATH
#define CMED_DEV_EXAMPLE_PATH ""
#endif

namespace
{

// Paths inside this run's own scratch directory, set once from main() before any case runs.
std::string g_clientScratch;
std::string g_daemonScratch;

// The least a daemon's file has to say for a requester to be loadable at all. Named once, because a
// case about a client key would otherwise be refused for the rendezvous it forgot to write.
constexpr const char* DaemonRendezvous = "area:\n  name: lane\nsocket:\n  dir: /run/probe\n";

// Written rather than parsed in memory, so a case takes the same load path a deployment does. The
// chmod is not setup: ofstream creates through the umask, and a reader refuses what group can write.
void writeRestricted(const std::string& path, const std::string& text)
{
    {
        std::ofstream file{path};
        file << text;
    }

    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)  // NOLINT(misc-include-cleaner)
    {
        throw cmed::CmedInvalidArgumentError{path + ": could not restrict the scratch file"};
    }
}

// No [[nodiscard]] on either loader: half the cases below call one for the throw and drop the value,
// and a cast at each of those says less than its absence here.
cmed::CmedClientConfig_t loadClientConfig(const std::string& own, const std::string& daemons)
{
    writeRestricted(g_clientScratch, own);
    writeRestricted(g_daemonScratch, daemons);

    const cmed::CmedClientConfig_t config = cmed::loadClientConfig(g_clientScratch, g_daemonScratch);
    std::remove(g_clientScratch.c_str());
    std::remove(g_daemonScratch.c_str());
    return config;
}

cmed::daemon::DaemonConfig_t loadDaemonConfig(const std::string& text)
{
    writeRestricted(g_daemonScratch, text);

    const cmed::daemon::DaemonConfig_t config = cmed::daemon::loadDaemonConfig(g_daemonScratch);
    std::remove(g_daemonScratch.c_str());
    return config;
}

// Whether a load was refused at all. Both refusal types count: a key the parser could not read and a
// value the reader will not act on are the same outcome for a deployment, which is a daemon that
// does not start rather than one running on a setting nobody wrote.
template <typename T_Body>
[[nodiscard]] bool refuses(T_Body&& body)
{
    try
    {
        body();
    }
    catch (const cmed::CmedError&)
    {
        return true;
    }
    catch (const kvconfig::ParseError&)
    {
        return true;
    }
    return false;
}

// Every deadline defaults, and the socket path cannot. So an application whose own file is absent
// still runs, and one whose daemon file is absent has nothing to connect to and is told so.
void anAbsentAppFileKeepsEveryDeadline(probe::Context& ctx)
{
    ctx.openCase("an application file that is not there");

    const cmed::CmedClientConfig_t config = loadClientConfig("", DaemonRendezvous);
    const cmed::CmedClientConfig_t defaults;

    ctx.check(config.socketPath == "/run/probe/lane.sock", "the socket path still comes from the daemon's file");
    ctx.check(config.setupTimeout == defaults.setupTimeout && config.lockTimeout == defaults.lockTimeout,
              "and every deadline keeps the struct's own default");
}

void everyClientKeyLandsInItsField(probe::Context& ctx)
{
    ctx.openCase("every key an application's file owns");

    const cmed::CmedClientConfig_t config = loadClientConfig(
        "client:\n"
        "  setup_timeout_ms: 1200\n"
        "  lock_timeout_ms: 300\n",
        DaemonRendezvous);

    ctx.check(config.setupTimeout == timing::Millis{1200}, "client.setup_timeout_ms lands in setupTimeout");
    ctx.check(config.lockTimeout == timing::Millis{300}, "client.lock_timeout_ms lands in lockTimeout");
    ctx.check(config.socketPath == "/run/probe/lane.sock", "and the socket path is unchanged by any of them");
}

// The daemon bound the socket at the path its own two keys make. An application that could name its
// own would be naming a socket nobody is listening on, so its file must not reach this field.
void theSocketPathIsTheDaemonsAlone(probe::Context& ctx)
{
    ctx.openCase("an application naming its own rendezvous");

    const cmed::CmedClientConfig_t config = loadClientConfig(
        "area:\n"
        "  name: mine\n"
        "socket:\n"
        "  dir: /run/mine\n",
        DaemonRendezvous);

    ctx.check(config.socketPath == "/run/probe/lane.sock",
              "the daemon's two keys win, and the application's copies of them reach nothing");
}

void everyDaemonKeyLandsInItsField(probe::Context& ctx)
{
    ctx.openCase("every key the daemon's file owns");

    const cmed::daemon::DaemonConfig_t config = loadDaemonConfig(
        "area:\n"
        "  name: lane\n"
        "socket:\n"
        "  dir: /run/probe\n"
        "  mode: \"0640\"\n"
        "region:\n"
        "  uri: shm:/other\n"
        "  coherency: uncached\n"
        "  format_timeout_ms: 900\n"
        "cohort:\n"
        "  hold_ms: 30\n"
        "  grant_validity_ms: 80\n"
        "workers:\n"
        "  count: 6\n"
        "  spin_us: 33\n"
        "maintenance:\n"
        "  interval_ms: 17\n"
        "serve:\n"
        "  spin_us: 44\n"
        "  idle_interval_ms: 50\n"
        "registry:\n"
        "  refresh_interval_ms: 700\n"
        "  delete_wait_timeout_ms: 90\n");

    ctx.check(config.area.name == "lane" && config.socket.mode == 0640, "area.name and socket.mode land in their fields");
    ctx.check(config.socketPath() == "/run/probe/lane.sock" && config.lockPath() == "/run/probe/lane.lock",
              "and the socket and the lock are built from that one pair, so both name the same run");
    ctx.check(config.region.uri == "shm:/other" && config.region.coherency == "uncached" &&
                  config.region.formatTimeout == timing::Millis{900},
              "the three region keys land in their fields");
    ctx.check(config.cohort.hold == timing::Millis{30} && config.cohort.grantValidity == timing::Millis{80},
              "both cohort keys land in theirs");

    // Every key the worker pool and the dispatcher read. Each of these parses whether or not anything
    // reads it back, so a key that landed nowhere would leave the compiled-in default in force.
    ctx.check(config.workers.count == 6 && config.serve.idleInterval == timing::Millis{50} &&
                  config.workers.spin == timing::Micros{33} && config.maintenance.interval == timing::Millis{17},
              "all four workers keys land in theirs");
    ctx.check(config.serve.spin == timing::Micros{44}, "serve.spin_us lands in serveSpin");
    ctx.check(config.registry.refreshInterval == timing::Millis{700} && config.registry.deleteWaitTimeout == timing::Millis{90},
              "and both registry keys land in theirs");
}

// Membership and not an index, because what admit asks for is a set of accounts. The order a file
// wrote them in is the reader's to keep or drop.
template <typename T_Account>
[[nodiscard]] bool admitsEvery(const std::vector<T_Account>& listed, std::initializer_list<T_Account> wanted)
{
    const std::unordered_set<T_Account> holding{listed.begin(), listed.end()};
    for (const T_Account account : wanted)
    {
        if (holding.count(account) == 0)
        {
            return false;
        }
    }
    return true;
}

// The accounts a socket admits. Nothing else in the tree reads this list, so a key that parsed and
// landed nowhere would leave a daemon admitting its own uid alone with no line anywhere saying so.
void theAdmitListsReachThePolicy(probe::Context& ctx)
{
    ctx.openCase("the accounts admit names");

    const cmed::daemon::DaemonConfig_t config = loadDaemonConfig(
        "area:\n"
        "  name: lane\n"
        "admit:\n"
        "  uids: [4001, 4002]\n"
        "  gids: [4003]\n");

    // Count beside membership, so a list that admits the two named accounts and a third one is refused
    // by this check rather than passing it.
    ctx.check(config.admit.uids.size() == 2 && admitsEvery<::uid_t>(config.admit.uids, {4001, 4002}),
              "both uids reach the policy and nothing else does");
    ctx.check(config.admit.gids.size() == 1 && admitsEvery<::gid_t>(config.admit.gids, {4003}),
              "and the gid beside them");

    const cmed::daemon::DaemonConfig_t neither = loadDaemonConfig("area:\n  name: lane\n");
    ctx.check(neither.admit.uids.empty() && neither.admit.gids.empty(),
              "a file naming neither admits this daemon's own uid alone");

    // The refusal quotes the key rather than the entry, since the key is what a deployment fixes. An
    // entry read as a number instead would be id zero, which is root.
    ctx.check(refuses([]
                      {
                          loadDaemonConfig("area:\n  name: lane\nadmit:\n  uids: [\"\"]\n");
                      }),
              "a blank entry is refused rather than resolved to root");
    ctx.check(refuses([]
                      {
                          loadDaemonConfig("area:\n  name: lane\nadmit:\n  gids: [4294967296]\n");
                      }),
              "and so is one past the id's own width");
}

// A key a reader does not own is parsed and dropped rather than refused. That is what lets one of
// these files gain a key for a future reader without the current one failing to start.
void unknownKeysAreDropped(probe::Context& ctx)
{
    ctx.openCase("a key the reader does not own");

    const cmed::CmedClientConfig_t asClient = loadClientConfig(
        "cohort:\n"
        "  hold_ms: 30\n"
        "client:\n"
        "  lock_timeout_ms: 25\n",
        DaemonRendezvous);

    const cmed::daemon::DaemonConfig_t asDaemon = loadDaemonConfig(
        "client:\n"
        "  lock_timeout_ms: 25\n"
        "cohort:\n"
        "  hold_ms: 30\n");
    const cmed::daemon::DaemonConfig_t daemonDefaults;

    ctx.check(asClient.lockTimeout == timing::Millis{25}, "a daemon key in an application's file is dropped");
    ctx.check(asDaemon.cohort.hold == timing::Millis{30} && asDaemon.serve.idleInterval == daemonDefaults.serve.idleInterval,
              "and an application key in the daemon's file is dropped too");
}

// A key the file leaves out keeps the struct's own default rather than a second copy of it written
// down in the reader.
void anAbsentKeyKeepsItsDefault(probe::Context& ctx)
{
    ctx.openCase("a key the file leaves out");

    const cmed::CmedClientConfig_t config = loadClientConfig("client:\n  lock_timeout_ms: 25\n", DaemonRendezvous);
    const cmed::CmedClientConfig_t defaults;

    ctx.check(config.lockTimeout == timing::Millis{25}, "the key that is there is read");
    ctx.check(config.setupTimeout == defaults.setupTimeout, "and the one that is not keeps the struct's default");
}

// Both halves of the socket path come from the daemon's file, so either one missing leaves nothing to
// connect to. Refused rather than joined into a path that looks real: `/.sock` is a path.
void aHalfNamedRendezvousIsRefused(probe::Context& ctx)
{
    ctx.openCase("a rendezvous named by half");

    ctx.check(refuses([]
                      {
                          loadClientConfig("", "area:\n  name: lane\n");
                      }),
              "an area name with no socket directory is refused");
    ctx.check(refuses([]
                      {
                          loadClientConfig("", "socket:\n  dir: /run/probe\n");
                      }),
              "a socket directory with no area name is refused");
    ctx.check(refuses([]
                      {
                          loadClientConfig("", "");
                      }),
              "and a daemon file naming neither is refused rather than composing /.sock");
}

// A zero timeout is a real answer: it says do not wait. What a reader refuses is a line it cannot
// read at all. Every case here writes the rendezvous, so what it refuses is its own key.
void anApplicationValueTheReaderWillNotActOn(probe::Context& ctx)
{
    ctx.openCase("a value an application's reader will not act on");

    ctx.check(!refuses([]
                       {
                           loadClientConfig("client:\n  lock_timeout_ms: 0\n", DaemonRendezvous);
                       }),
              "a zero lock timeout is taken, since it says do not wait");
    ctx.check(refuses([]
                      {
                          loadClientConfig("client\n  lock_timeout_ms: 7\n", DaemonRendezvous);
                      }),
              "a line the parser cannot read is refused");
    ctx.check(refuses([]
                      {
                          loadClientConfig("client:\n  lock_timeout_ms: soon\n", DaemonRendezvous);
                      }),
              "and so is a duration that is not a number");
}

void aDaemonValueTheReaderWillNotActOn(probe::Context& ctx)
{
    ctx.openCase("a value the daemon's reader will not act on");

    ctx.check(refuses([]
                      {
                          loadClientConfig("", "area:\n  name: \"\"\nsocket:\n  dir: /run/probe\n");
                      }),
              "an empty area name is refused");
    ctx.check(refuses([]
                      {
                          loadDaemonConfig("region:\n  uri: \"\"\n");
                      }),
              "an empty region uri is refused");
    ctx.check(refuses([]
                      {
                          loadDaemonConfig("serve:\n  idle_interval_ms: 0\n");
                      }),
              "a zero idle turn is refused, since it is a spin");

    // Both sides of the bound, so a run that stopped refusing the one past it would fail this rather
    // than start with a staleness window wider than the field that carries it.
    ctx.check(refuses([]
                      {
                          loadDaemonConfig("serve:\n  idle_interval_ms: 60001\n");
                      }),
              "an idle turn one past the bound is refused");
    ctx.check(!refuses([]
                       {
                           loadDaemonConfig("serve:\n  idle_interval_ms: 60000\n");
                       }),
              "and the bound itself is taken");

    // A requester that connects can write any word of the area it receives, so the socket's mode is
    // the whole admission boundary the filesystem draws.
    ctx.check(refuses([]
                      {
                          loadDaemonConfig("socket:\n  mode: \"0666\"\n");
                      }),
              "a world-writable socket mode is refused");
}

// A run has to end while the grants it issued are still worth acting on. The two keys are read
// separately and only make sense against each other, so the reader compares them.
void aHoldThatOutlastsItsGrantIsRefused(probe::Context& ctx)
{
    ctx.openCase("a hold against the validity it is judged by");

    ctx.check(refuses([]
                      {
                          loadDaemonConfig("cohort:\n  hold_ms: 400\n  grant_validity_ms: 100\n");
                      }),
              "a hold longer than the grant validity is refused");
    ctx.check(refuses([]
                      {
                          loadDaemonConfig("cohort:\n  hold_ms: 100\n  grant_validity_ms: 100\n");
                      }),
              "and so is one exactly equal to it, since the turn would still be held at that instant");
    ctx.check(!refuses([]
                       {
                           loadDaemonConfig("cohort:\n  hold_ms: 99\n  grant_validity_ms: 100\n");
                       }),
              "one millisecond under is taken");

    // Written apart, so a file naming one of the two is judged against the other's default rather
    // than against nothing. The default pair has to pass its own rule.
    ctx.check(refuses([]
                      {
                          loadDaemonConfig("cohort:\n  hold_ms: 5000\n");
                      }),
              "and a hold named alone is judged against the compiled-in validity");
}

[[nodiscard]] std::string readContents(const char* path)
{
    std::ifstream shipped{path};
    return std::string{std::istreambuf_iterator<char>{shipped}, std::istreambuf_iterator<char>{}};
}

// Parsed, not just present: a stale example is a reader copying a key that lands nowhere. Through a
// copy, because a checkout carries whatever the umask gave it and a reader refuses group-write.
void theShippedExamplesStillParse(probe::Context& ctx)
{
    ctx.openCase("the examples this build ships");

    const std::string daemonText = readContents(CMED_DAEMON_EXAMPLE_PATH);
    const std::string clientText = readContents(CMED_CLIENT_EXAMPLE_PATH);
    if (!ctx.check(!daemonText.empty() && !clientText.empty(), "both example files were found and read"))
    {
        return;
    }

    const cmed::daemon::DaemonConfig_t asDaemon = loadDaemonConfig(daemonText);
    const cmed::CmedClientConfig_t asClient = loadClientConfig(clientText, daemonText);

    ctx.check(asDaemon.area.name == "cmed" && asDaemon.socketPath() == "/run/cmed/cmed.sock" &&
                  asDaemon.region.uri == "shm:/cme-region",
              "the daemon's example lands where it claims");
    ctx.check(asClient.socketPath == "/run/cmed/cmed.sock", "the application reaches the same socket");
    ctx.check(asClient.setupTimeout == timing::Secs{5} && asClient.lockTimeout == timing::Secs{5},
              "and the deadlines it documents are the ones it produces");

    // The development copy is the one a reader of the tree actually starts a daemon with, so it is
    // held to the same reader rather than left to rot beside the file it was copied from.
    const std::string devText = readContents(CMED_DEV_EXAMPLE_PATH);
    if (!ctx.check(!devText.empty(), "the development example was found and read"))
    {
        return;
    }

    const cmed::daemon::DaemonConfig_t asDev = loadDaemonConfig(devText);
    ctx.check(asDev.socketPath() == "/tmp/cmed/cmed.sock" && asDev.region.uri == "shm:/cme-region",
              "the development example needs no root and no device");
}

}  // namespace

int main()
{
    const cmed::harness::ProbeScratch scratch{"config-probe"};
    g_clientScratch = scratch.makePath("client.yaml");
    g_daemonScratch = scratch.makePath("daemon.yaml");

    return probe::run("config probe",
                      [](probe::Context& ctx)
                      {
                          anAbsentAppFileKeepsEveryDeadline(ctx);
                          everyClientKeyLandsInItsField(ctx);
                          theSocketPathIsTheDaemonsAlone(ctx);
                          everyDaemonKeyLandsInItsField(ctx);
                          theAdmitListsReachThePolicy(ctx);
                          unknownKeysAreDropped(ctx);
                          anAbsentKeyKeepsItsDefault(ctx);
                          aHalfNamedRendezvousIsRefused(ctx);
                          anApplicationValueTheReaderWillNotActOn(ctx);
                          aDaemonValueTheReaderWillNotActOn(ctx);
                          aHoldThatOutlastsItsGrantIsRefused(ctx);
                          theShippedExamplesStillParse(ctx);
                      });
}
