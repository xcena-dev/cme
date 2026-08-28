// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_served_daemon.hpp -- one real daemon process a case can start, kill and start again.
//
// Apart from helper_daemon.hpp, which writes the words a daemon would write without being one. This
// execs the binary, so what a case here asks is what the shipped daemon does with a signal or a
// boundary. A case that only needs the words wants the other one.
//
// The including target must define CMED_BINARY_PATH, since a probe reaches the daemon by path.

#pragma once

// The target passes the path as a macro, so this is where it stops being one. The fallback is only so
// the header compiles on its own, which the style gate makes it do: a probe built without the real
// definition execs a path that is not there and exits 127, rather than a daemon it meant.
#if !defined(CMED_BINARY_PATH)
#define CMED_BINARY_PATH "cmed-binary-path-was-not-defined"  // NOLINT(cppcoreguidelines-macro-usage)
#endif

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "cmed/config.hpp"
#include "cmed/errors.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/protocol/socket_path.hpp"

namespace cmed::harness
{

// Named once, so nothing below spells the macro and a reader sees a string rather than a definition.
inline constexpr const char* DaemonBinaryPath = CMED_BINARY_PATH;

// Long enough for a daemon inside a region acquire to answer a signal, short enough that a case which
// cannot be stopped politely still reports rather than hanging.
inline constexpr timing::Millis DaemonStopWait{1000};

// The gap between looks while waiting for a daemon to answer at all.
inline constexpr timing::Millis DaemonStartPoll{20};

// How long a probe allows for a daemon to bind its socket. Startup only, so a case that waits this
// long has a daemon that did not come up rather than one that is slow to answer.
inline constexpr timing::Millis DaemonBindWait{2000};

// ── one daemon's files and addresses ────────────────────────────────────
// Every probe that runs a daemon needs these four, and a probe that spelled them itself would be
// naming the same paths the daemon composes from its own config.

[[nodiscard]] inline std::string daemonConfigPath(const ProbeScratch& scratch, std::string_view areaName)
{
    return scratch.makePath(std::string{areaName} + ".yaml");
}

[[nodiscard]] inline std::string daemonSocketPath(const ProbeScratch& scratch, std::string_view areaName)
{
    return protocol::buildSocketPath(scratch.readDirectory(), std::string{areaName});
}

// What a probe writes for one daemon. The keys every probe sets are fields; whatever else its own axis
// needs goes in @extra verbatim, since no two probes want the same set of those.
struct DaemonSite_t
{
    std::string areaName;
    std::string uri;
    std::string coherency{"cache_coherent"};
    std::string extra;
};

// The chmod is not tidiness. ofstream creates through the umask, and the daemon refuses a config file
// anyone but its owner can write, because whoever can rewrite it chooses the region it serves.
inline void writeDaemonConfig(const ProbeScratch& scratch, const DaemonSite_t& site)
{
    const std::string path = daemonConfigPath(scratch, site.areaName);
    {
        std::ofstream file{path};
        file << "socket:\n"
             << "  dir: " << scratch.readDirectory() << "\n"
             << "  mode: \"0600\"\n"
             << "area:\n"
             << "  name: " << site.areaName << "\n"
             << "region:\n"
             << "  uri: " << site.uri << "\n"
             << "  coherency: " << site.coherency << "\n"
             << site.extra;
    }

    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)  // NOLINT(misc-include-cleaner) POSIX, via <sys/stat.h>
    {
        throw cmed::CmedInvalidArgumentError{path + ": could not restrict the config file"};
    }
}

[[nodiscard]] inline cmed::CmedClientConfig_t
clientConfigFor(const ProbeScratch& scratch, std::string_view areaName, timing::Millis setup,
                timing::Millis lock)
{
    cmed::CmedClientConfig_t config;
    config.socketPath = daemonSocketPath(scratch, areaName);
    config.setupTimeout = setup;
    config.lockTimeout = lock;
    return config;
}

// A connect that raced the bind would be refused for the wrong reason, so this is what says the daemon
// is reachable before a probe drives anything through it.
[[nodiscard]] inline bool awaitDaemonBound(const ProbeScratch& scratch, std::string_view areaName,
                                           timing::Millis within)
{
    const std::string path = daemonSocketPath(scratch, areaName);
    return poll::waitUntil(
        [&path]
        {
            return ::access(path.c_str(), F_OK) == 0;
        },
        within, DaemonStartPoll);
}

class NodeDaemon
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // @armed is the boundary name a CMED_FAILPOINT build dies at, empty for a run that dies nowhere.
    // A build without the axis refuses a name outright rather than ignoring it.
    explicit NodeDaemon(std::string configPath, std::string armed = {})
        : configPath_{std::move(configPath)},
          armed_{std::move(armed)},
          running_{spawn(configPath_, armed_)}
    {
    }

    NodeDaemon(const NodeDaemon&) = delete;
    NodeDaemon(NodeDaemon&&) = delete;

    // Stops whatever is running, so a leftover daemon is not left squatting on the build tree's
    // socket for the next probe.
    ~NodeDaemon()
    {
        stop(SIGTERM);
    }

    // ── operator= ──────────────────────────────────────────────────────
    NodeDaemon& operator=(const NodeDaemon&) = delete;
    NodeDaemon& operator=(NodeDaemon&&) = delete;

    // ── public methods ─────────────────────────────────────────────────
    // Reaped here rather than left to the destructor, because a case that killed a daemon and started
    // another needs the first one gone before the second takes its lock file.
    void kill() noexcept
    {
        stop(SIGKILL);  // NOLINT(misc-include-cleaner) POSIX, via <csignal>
    }

    // Stops whatever is running first. Overwriting a live pid would leave a daemon nothing can reach,
    // still holding the area and the stdout its reader waits on for EOF.
    [[nodiscard]] bool start()
    {
        stop(SIGTERM);
        running_ = spawn(configPath_, armed_);
        return running_ > 0;
    }

    // Waits for this daemon to leave on its own, which is what an armed boundary makes it do. False
    // when it was still running at @within, which is a boundary the run never reached.
    [[nodiscard]] bool awaitGone(timing::Millis within) noexcept
    {
        if (running_ <= 0)
        {
            return true;
        }

        int status = 0;
        const bool reaped = poll::waitUntil(
            [this, &status]
            {
                return ::waitpid(running_, &status, WNOHANG) != 0;
            },
            within, DaemonStartPoll);
        if (reaped)
        {
            running_ = 0;
        }
        return reaped;
    }

    // ── accessors ──────────────────────────────────────────────────────
    [[nodiscard]] bool started() const noexcept
    {
        return running_ > 0;
    }

private:
    [[nodiscard]] static ::pid_t spawn(const std::string& configPath, const std::string& armed)
    {
        const ::pid_t child = ::fork();
        if (child != 0)
        {
            return child;
        }

        if (armed.empty())
        {
            static_cast<void>(::execl(DaemonBinaryPath, DaemonBinaryPath,
                                      "--config", configPath.c_str(), nullptr));
        }
        else
        {
            static_cast<void>(::execl(DaemonBinaryPath, DaemonBinaryPath,
                                      "--config", configPath.c_str(),
                                      "--arm", armed.c_str(), nullptr));
        }
        std::_Exit(127);
    }

    // SIGKILL after a bounded wait, because a daemon asked to stop may be inside a region acquire on a
    // domain a killed peer still holds, and a probe cannot wait out that acquire to finish reporting.
    void stop(int signal) noexcept
    {
        if (running_ <= 0)
        {
            return;
        }

        ::kill(running_, signal);  // NOLINT(misc-include-cleaner) POSIX, via <csignal>

        int status = 0;
        const bool reaped = poll::waitUntil(
            [this, &status]
            {
                return ::waitpid(running_, &status, WNOHANG) != 0;
            },
            DaemonStopWait, DaemonStartPoll);
        if (!reaped)
        {
            static_cast<void>(::kill(running_, SIGKILL));
            static_cast<void>(::waitpid(running_, &status, 0));
        }
        running_ = 0;
    }

    std::string configPath_;
    std::string armed_;
    ::pid_t running_;
};

// One daemon armed at a named boundary, started from a clean socket and waited for. Its own type
// because the order matters: the stale socket goes first, then the start, then the wait.
//
// probe::Context is not named here, so a case checks what this reports rather than this recording a
// verdict of its own.
class ArmedDaemon
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    ArmedDaemon(const ProbeScratch& scratch, std::string_view areaName, std::string armed)
        : running_{startFrom(scratch, areaName, std::move(armed))},
          bound_{running_.started() && awaitDaemonBound(scratch, areaName, DaemonBindWait)}
    {
    }

    ArmedDaemon(const ArmedDaemon&) = delete;
    ArmedDaemon(ArmedDaemon&&) = delete;
    ~ArmedDaemon() = default;

    // ── operator= ──────────────────────────────────────────────────
    ArmedDaemon& operator=(const ArmedDaemon&) = delete;
    ArmedDaemon& operator=(ArmedDaemon&&) = delete;

    // ── public methods ─────────────────────────────────────────────
    // Whether it left at the boundary rather than serving on, which is every armed case's last check.
    [[nodiscard]] bool awaitGone(timing::Millis within) noexcept
    {
        return running_.awaitGone(within);
    }

    // ── accessors ──────────────────────────────────────────────────
    // Started and listening, which is what a case checks before it drives anything.
    [[nodiscard]] bool serving() const noexcept
    {
        return bound_;
    }

private:
    // A daemon killed at a boundary leaves its socket file behind, so it goes before the start and the
    // wait then means this daemon bound it rather than that the last one did.
    [[nodiscard]] static NodeDaemon startFrom(const ProbeScratch& scratch, std::string_view areaName,
                                              std::string armed)
    {
        static_cast<void>(::unlink(daemonSocketPath(scratch, areaName).c_str()));
        return NodeDaemon{daemonConfigPath(scratch, areaName), std::move(armed)};
    }

    NodeDaemon running_;
    bool bound_;
};

}  // namespace cmed::harness
