// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// startup_probe.cpp -- the daemon binary starts, says it is serving, and stops.
//
// The only probe that execs cmed; every other one assembles the daemon's pieces inside its own process
// and never reaches main(). Raw fork and execve rather than harness/helper_process.hpp, since that forks
// a body in this program's image, and what is under test here is the built binary and the codes it
// exits with.
//
// Registered once per medium: --backend and --slot name one and the site config resolves it, while
// --uri names one whole and leaves it the caller's.

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "harness/helper_medium.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/posix/datagram_socket.hpp"
#include "shared/posix/seqpacket_socket.hpp"
#include "test_memory.hpp"
#include "test_options.hpp"

extern char** environ;

namespace
{

// Generous, because what is being timed is a fork, an exec and a region open on a loaded CI host.
// The probe's own ctest timeout is the real ceiling.
constexpr timing::Millis ReadyWait{10000};
constexpr timing::Millis ExitWait{5000};
constexpr timing::Millis PollStep{5};

// Long enough that a daemon which exits right after saying READY=1 is caught, short enough to add
// nothing to a passing run.
constexpr timing::Millis SettleWait{200};

// What main() returns when another daemon already serves this area. The unit file turns this into
// RestartPreventExitStatus, so a change here is a change to a deployment contract.
constexpr std::int32_t AlreadyServingExit = 3;

// Long enough for READY=1 and the status line beside it, and short enough that a daemon sending
// something else is caught by the check rather than by a truncation.
constexpr std::uint32_t DatagramMost = 512;

// A page, which covers the header line a region's magic sits in with room to spare.
constexpr std::uint64_t ScrubBytes = 4096;

// The last case needs an area nobody formatted, and a dax window keeps whatever the run before left
// in it. Zeroing the header is the removal a device offers no other form of.
void scrubHeader(harness::TestMemory& area)
{
    std::memset(area.map(ScrubBytes), 0, static_cast<std::size_t>(ScrubBytes));
}

// Absolute, because a service manager passes NOTIFY_SOCKET as an absolute path or an abstract name
// and the daemon refuses anything else. A relative one would be a probe that tests nothing.
[[nodiscard]] std::string notifyPathAbsolute(const cmed::harness::ProbeScratch& scratch)
{
    return std::filesystem::absolute(scratch.makePath("ready.notify")).string();
}

void writeConfig(const char* path, const std::string& areaName, const std::string& socketDir,
                 const std::string& regionUri, const std::string& coherency)
{
    std::ofstream file{path, std::ios::trunc};
    file << "area:\n"
         << "  name: " << areaName << "\n"
         // The daemon binds its socket here rather than under /run, which this uid cannot write. The
         // mode belongs to the socket: the daemon reads no mode of the area's.
         << "socket:\n"
         << "  dir: " << socketDir << "\n"
         << "  mode: \"0600\"\n"
         << "region:\n"
         << "  uri: " << regionUri << "\n"
         << "  coherency: " << coherency << "\n";
    file.close();

    // The daemon refuses a config anyone else may write, and the umask ctest runs under is not this
    // probe's to assume.
    static_cast<void>(::chmod(path, 0600));
}

// Built before the fork, so the child between fork and exec calls nothing but execve. Anything
// allocating there would be running in a copy of a process that may hold libcme's poll thread.
[[nodiscard]] std::vector<std::string> environmentWith(const std::string& added)
{
    std::vector<std::string> carried;
    for (char** entry = ::environ; *entry != nullptr; ++entry)
    {
        if (std::string_view{*entry}.substr(0, std::strlen("NOTIFY_SOCKET=")) != "NOTIFY_SOCKET=")
        {
            carried.emplace_back(*entry);
        }
    }
    carried.push_back(added);
    return carried;
}

[[nodiscard]] ::pid_t spawnDaemon(const char* configPath, const std::string& notifyPath)
{
    std::vector<std::string> environment = environmentWith("NOTIFY_SOCKET=" + notifyPath);
    std::vector<char*> passed;
    passed.reserve(environment.size() + 1);
    for (std::string& entry : environment)
    {
        passed.push_back(entry.data());
    }
    passed.push_back(nullptr);

    std::string binary{CMED_BINARY_PATH};
    std::string flag{"--config"};
    std::string named{configPath};
    char* arguments[] = {binary.data(), flag.data(), named.data(), nullptr};

    const ::pid_t child = ::fork();
    if (child != 0)
    {
        return child;
    }

    static_cast<void>(::execve(binary.c_str(), arguments, passed.data()));
    std::_Exit(127);
}

// The exit code, or the negated signal that ended it. Nothing means it outlived @limit.
[[nodiscard]] std::optional<std::int32_t> waitFor(::pid_t child, timing::Millis limit)
{
    for (timing::Millis waited{0}; waited < limit; waited += PollStep)
    {
        std::int32_t status = 0;
        const ::pid_t seen = ::waitpid(child, &status, WNOHANG);
        if (seen < 0)
        {
            return std::nullopt;
        }
        if (seen == child)
        {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
        }
        std::this_thread::sleep_for(PollStep);
    }
    return std::nullopt;
}

// Whether anything claimed readiness for @carrying inside @within, an empty name matching any claim.
// Every daemon this probe spawns notifies the one receiver, and sd_notify fixes neither the number of
// datagrams nor their order, so anything that is not the claim is read past rather than judged.
[[nodiscard]] bool claimedReady(posix::DatagramSocket& notified, timing::Millis within, const std::string& carrying)
{
    const timing::Deadline listening{within};
    while (!listening.expired())
    {
        const std::optional<std::string> payload = notified.receive(PollStep, DatagramMost);
        if (payload && payload->find("READY=1") != std::string::npos && payload->find(carrying) != std::string::npos)
        {
            std::printf("claimed %s\n", payload->c_str());
            return true;
        }
    }
    return false;
}

// READY=1 says the area is published and the liveness mutex held. The area name is read with it, or a
// datagram from another run would pass this case.
[[nodiscard]] bool saidReady(posix::DatagramSocket& notified, ::pid_t serving, const std::string& areaName)
{
    if (!claimedReady(notified, ReadyWait, areaName))
    {
        std::printf("no READY=1 for %s within %" PRId64 " ms\n", areaName.c_str(), ReadyWait.count());
        return false;
    }

    // Said it and meant it: a daemon that exits here reported a readiness it did not have.
    std::this_thread::sleep_for(SettleWait);
    if (const std::optional<std::int32_t> ended = waitFor(serving, PollStep))
    {
        std::printf("exited %d just after READY=1\n", *ended);
        return false;
    }
    return true;
}

// READY=1 says a requester can be answered, and reaching the socket is half of what that means. The
// path comes from the config, so this also holds the daemon to binding where it was told to.
[[nodiscard]] bool boundASocketToConnectTo(const std::string& socketPath)
{
    struct ::stat found = {};
    if (::stat(socketPath.c_str(), &found) != 0 || !S_ISSOCK(found.st_mode))
    {
        std::printf("no socket at %s\n", socketPath.c_str());
        return false;
    }

    try
    {
        const auto reaching = posix::SeqpacketSocket::connect(socketPath);
        return static_cast<bool>(reaching);
    }
    catch (const std::exception& failure)
    {
        std::printf("could not reach %s: %s\n", socketPath.c_str(), failure.what());
        return false;
    }
}

// Two daemons on one area is a deployment mistake, and the exit code says so distinctly enough that
// a supervisor can decline to restart. Nothing is sent, because nothing was served.
[[nodiscard]] bool refusedASecondDaemon(posix::DatagramSocket& notified, const std::string& configPath)
{
    const ::pid_t second = spawnDaemon(configPath.c_str(), notified.name());
    if (second < 0)
    {
        return false;
    }

    const std::optional<std::int32_t> ended = waitFor(second, ExitWait);
    if (!ended)
    {
        static_cast<void>(::kill(second, SIGKILL));  // NOLINT(misc-include-cleaner) POSIX, via <csignal>
        static_cast<void>(waitFor(second, ExitWait));
        std::printf("second daemon kept running on a served area\n");
        return false;
    }
    if (*ended != AlreadyServingExit)
    {
        std::printf("second daemon exited %d, expected %d\n", *ended, AlreadyServingExit);
        return false;
    }
    if (claimedReady(notified, PollStep, ""))
    {
        std::printf("the second daemon claimed readiness on a served area\n");
        return false;
    }
    return true;
}

// A signal ends the loop rather than the process, which is what unlinks the area and releases the
// cme peer slot, so the code has to be 0 for a supervisor to read the stop as intended.
[[nodiscard]] bool stopsOnSignal(::pid_t serving, const std::string& socketPath)
{
    if (::kill(serving, SIGTERM) != 0)
    {
        return false;
    }

    const std::optional<std::int32_t> ended = waitFor(serving, ExitWait);
    if (!ended)
    {
        static_cast<void>(::kill(serving, SIGKILL));
        static_cast<void>(waitFor(serving, ExitWait));
        std::printf("did not stop within %" PRId64 " ms of SIGTERM\n", ExitWait.count());
        return false;
    }
    if (*ended != 0)
    {
        std::printf("stopped with %d, expected 0\n", *ended);
        return false;
    }

    // The name goes with the daemon. One left behind is a path a requester can connect to and wait on
    // for an answer that is never coming.
    struct ::stat found = {};
    if (::stat(socketPath.c_str(), &found) == 0)
    {
        std::printf("%s outlived the daemon\n", socketPath.c_str());
        return false;
    }
    return true;
}

// The other half of the readiness contract. A daemon that cannot open its region must not announce,
// because an announcement is what makes a service manager stop waiting and report success.
[[nodiscard]] bool stayedQuietWhenItCannotStart(posix::DatagramSocket& notified, const std::string& absentConfigPath)
{
    const ::pid_t refusing = spawnDaemon(absentConfigPath.c_str(), notified.name());
    if (refusing < 0)
    {
        return false;
    }

    const std::optional<std::int32_t> ended = waitFor(refusing, ExitWait);
    if (!ended)
    {
        static_cast<void>(::kill(refusing, SIGKILL));
        static_cast<void>(waitFor(refusing, ExitWait));
        std::printf("kept running on a region nobody formatted\n");
        return false;
    }
    if (*ended == 0)
    {
        std::printf("exited 0 on a region nobody formatted\n");
        return false;
    }
    if (claimedReady(notified, PollStep, ""))
    {
        std::printf("claimed readiness while failing to start\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    cmed::harness::ProbeScratch scratch{"startup-probe"};

    cmed::harness::MediumOptions_t chosen;
    chosen.parse(argc, argv);

    const std::string areaName = scratch.makeAreaName("");
    const std::string socketDir = scratch.readDirectory();
    const std::string socketPath = socketDir + "/" + areaName + ".sock";
    const std::string configPath = scratch.makePath("config.yaml");
    const std::string absentConfigPath = scratch.makePath("absent.yaml");

    // Both are held for the whole run, because their destructors are what take the areas away. The
    // served one is empty when --uri named a medium the caller owns; the second is never formatted.
    std::unique_ptr<harness::TestMemory> served;
    std::unique_ptr<harness::TestMemory> unformatted;
    try
    {
        served = chosen.resolve();
        unformatted = chosen.openArea("absent", 1);
        scrubHeader(*unformatted);
    }
    catch (const harness::MediumUnavailable& absent)
    {
        std::printf("SKIP: %s\n", absent.what());
        return harness::SkipExitCode;
    }

    bool passed = false;
    try
    {
        // Formatted and not opened: the daemon is the peer here, and a session held by this probe
        // would occupy a slot no deployment has.
        cme::Session::FormatOpts_t opts;
        opts.maxDomains = 4;
        opts.maxPeers = 2;
        cme::Session::format(chosen.uri, opts);

        writeConfig(configPath.c_str(), areaName, socketDir, chosen.uri, chosen.coherency);
        writeConfig(absentConfigPath.c_str(), areaName, socketDir, unformatted->uri(),
                    chosen.coherency);

        auto notified = posix::DatagramSocket::receiver(notifyPathAbsolute(scratch));

        const ::pid_t serving = spawnDaemon(configPath.c_str(), notified.name());
        if (serving < 0)
        {
            throw std::runtime_error{"fork failed"};
        }

        // Named rather than chained, so the SIGTERM case runs even when an earlier one fails and the
        // one child this probe started is reaped exactly once.
        const bool ready = saidReady(notified, serving, areaName);
        const bool reachable = ready && boundASocketToConnectTo(socketPath);
        const bool refused = ready && refusedASecondDaemon(notified, configPath);
        const bool stopped = stopsOnSignal(serving, socketPath);
        const bool quiet = stayedQuietWhenItCannotStart(notified, absentConfigPath);

        passed = ready && reachable && refused && stopped && quiet;
    }
    catch (const std::exception& failure)
    {
        std::printf("startup probe threw: %s\n", failure.what());
        passed = false;
    }

    return passed ? 0 : 1;
}
