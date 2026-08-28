// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/main.cpp -- the daemon: one cme peer slot, serving every requester on this node. Two
// loops: this thread runs the control epoll, a second runs the serve loop's futex wait on the
// doorbell. A signal stops both cleanly, releasing the cme peer slot rather than killing the process.

// sigaction is POSIX and <csignal> does not declare it, so the C header is the one that has it.
#include <signal.h>  // NOLINT(modernize-deprecated-headers)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "daemon/control/handler.hpp"
#include "daemon/domain/manager.hpp"
#include "daemon/observe/failpoint.hpp"
#include "daemon/serve/handler.hpp"
#include "daemon/startup/config.hpp"
#include "daemon/startup/served_area.hpp"
#include "shared/area.hpp"
#include "shared/posix/lock_file.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

// Where a signal handler reads the loop it may end. Publishing and turning are one call, because a
// loop published and never turned would leave a stop with nothing to reach.
//
// A stop can land before any loop is published, so it is remembered and applied at the publication.
// Ordering the publication ahead of everything else would narrow that window rather than close it.
class StopSignal
{
public:
    // Turns @control until a stop signal reaches it, published for exactly that long. A throw between
    // the two stores would otherwise leave a pointer to a destroyed loop behind.
    static void runUntilStopped(cmed::daemon::ControlHandler& control)
    {
        const StopSignal reachable{control};
        control.run();
    }

    // All a signal handler may do here. The flag goes down first and stays down, so a stop that arrives
    // before any loop is published is remembered rather than lost.
    static void stop() noexcept
    {
        asked_.store(true, std::memory_order_release);

        const cmed::daemon::ControlHandler* const control = handler_.load(std::memory_order_acquire);
        if (control != nullptr)
        {
            control->stop();
        }
    }

    // ── ctor / dtor ────────────────────────────────────────────────
    StopSignal(const StopSignal&) = delete;
    StopSignal(StopSignal&&) = delete;

    // ── operator= ──────────────────────────────────────────────────
    StopSignal& operator=(const StopSignal&) = delete;
    StopSignal& operator=(StopSignal&&) = delete;

private:
    // The one that publishes, private so the publication cannot begin anywhere but the call above that
    // also ends it.
    explicit StopSignal(const cmed::daemon::ControlHandler& control) noexcept
    {
        handler_.store(&control, std::memory_order_release);

        // A stop that landed before this store found nothing to reach, so it is applied here instead.
        // Ending a loop that has not begun is fine: the eventfd it posts to is already there.
        if (asked_.load(std::memory_order_acquire))
        {
            control.stop();
        }
    }

    ~StopSignal() noexcept
    {
        handler_.store(nullptr, std::memory_order_release);
    }

private:
    // A pointer and not the loop itself: one run publishes one loop and takes it back on the way out.
    inline static std::atomic<const cmed::daemon::ControlHandler*> handler_{nullptr};

    // Whether a stop has been asked for at all. Never cleared: one run is what this process does, so a
    // stop asked before that run began is still a stop asked of it.
    inline static std::atomic<bool> asked_{false};
};

extern "C" void onStopSignal(int)
{
    StopSignal::stop();
}

// No SA_RESTART: a wait the signal lands in returns rather than resuming. The stop itself travels as
// an eventfd post, so both loops reach their top whichever thread took the signal.
void catchStopSignals()
{
    struct sigaction stopping = {};
    stopping.sa_handler = onStopSignal;
    ::sigemptyset(&stopping.sa_mask);

    ::sigaction(SIGINT, &stopping, nullptr);
    ::sigaction(SIGTERM, &stopping, nullptr);
}

[[nodiscard]] std::string readConfigPath(const std::vector<std::string_view>& arguments)
{
    for (std::uint32_t index = 1; index + 1 < arguments.size(); ++index)
    {
        if (arguments[index] == "--config")
        {
            return std::string{arguments[index + 1]};
        }
    }
    return cmed::daemon::DefaultConfigPath;
}

// One daemon's whole run: the region, the area, both loops, and the counters on the way out. Returns
// when SIGINT or SIGTERM has reached the control loop, which stops the serve threads with it.
void run(const cmed::daemon::DaemonConfig_t& config)
{
    // Held for the whole run: the region, the area, and the liveness mutex. Every line below is
    // answered out of it.
    cmed::daemon::ServedArea served{config};

    // Fills itself before the socket is bound. A requester that connected first would resolve nothing
    // and read that as a domain that does not exist rather than as a daemon that is not ready.
    cmed::daemon::DomainManager domainsManager{served.session(), served.shared(), config};

    // The bind, and the first thing here that can fail on a deployment's own mistake. Above the serve
    // threads, so a socket path this daemon cannot take costs no thread to start and join.
    cmed::daemon::ControlHandler control{config, served.area().descriptor(), served.shared().getAreaBytes(),
                                         domainsManager};

    // Last, because it announces once its two passes are up, and readiness claims the socket above too.
    const cmed::daemon::ServeHandler threads{served.shared(), domainsManager, config};

    StopSignal::runUntilStopped(control);
}

}  // namespace

int main(int argc, char** argv)
{
    catchStopSignals();

    try
    {
        // argv reaches nothing below this line: what the rest of the program passes around is a range of
        // views, and the C array stops at the one call that turns it into one.
        const std::vector<std::string_view> arguments{argv, argv + argc};

        const auto config = cmed::daemon::loadDaemonConfig(readConfigPath(arguments));

        // Before the lock, so a name this build cannot reach is refused without touching the area.
        if (!cmed::failpoint::arm(arguments))
        {
            std::fprintf(stderr, "cmed: --arm names no boundary this build compiled\n");
            return 2;
        }

        // Held for the whole run, and before anything else exists. The kernel arbitrates, so two
        // daemons launched at one instant cannot both find the name free the way two checks would.
        const auto lockPath = config.lockPath();
        const auto owning = posix::LockFile::take(lockPath);

        // A distinct code, because a second daemon on one area is a deployment mistake and not a
        // failure to start. A supervisor that restarts on 1 should not restart on this.
        if (!owning)
        {
            std::fprintf(stderr, "cmed: area %s is already served (lock held on %s)\n",
                         config.area.name.c_str(), lockPath.c_str());
            return 3;
        }

        run(config);
    }
    catch (const std::exception& failure)
    {
        std::fprintf(stderr, "cmed: %s\n", failure.what());
        return 1;
    }

    return 0;
}
