// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// session_config_probe.cpp -- the settings a caller passes reach the calls that wait on them.
//
// Parsing a field correctly is not the same as a call site reading it: a field that parses but is
// never read fails quietly, and the caller falls back to the compiled-in default with no line
// anywhere saying so. Each case here sets one field far from its default and reads the difference
// back off the clock.

#include <sys/types.h>

#include <cinttypes>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#include "cmed/config.hpp"
#include "cmed/errors.hpp"
#include "cmed/guard.hpp"
#include "cmed/session.hpp"
#include "common/timing.hpp"
#include "harness/helper.hpp"
#include "harness/helper_requester.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/posix/seqpacket_socket.hpp"

namespace
{

// A path in this probe's own directory that nothing binds. Absent rather than refusing, which is the
// same "not yet" a daemon still starting gives.
constexpr const char* UnboundSocket = "./cmed-session-config-probe-nobody.sock";

constexpr const char* DomainName = "lane0";

constexpr std::uint32_t DomainSlot = cmed::harness::FirstDataSlot;

// Far enough below the compiled-in 5000 ms that a default leaking through cannot land inside the
// window, and far enough above the scheduler's noise that a correct run cannot fall below it.
constexpr timing::Millis ShortWait{80};
constexpr timing::Millis Ceiling{2000};

// The listening socket a case binds to be silent on. One waiting connection is all any case makes.
constexpr ::mode_t SocketMode = 0600;
constexpr std::int32_t Backlog = 1;

// Every deadline at once, because a case reads one of them and the others only have to be far from
// their defaults for a leak to show.
[[nodiscard]] cmed::CmedClientConfig_t shortDeadlines()
{
    cmed::CmedClientConfig_t config;
    config.lockTimeout = ShortWait;
    config.setupTimeout = ShortWait;
    return config;
}

// lock() takes its deadline from the session's config rather than a constant of its own, so a
// daemon that never grants must give up at the configured wait and not at the default.
void theLockTimeoutReachesLock(probe::Context& ctx, cmed::harness::ProbeArea& area,
                               const cmed::harness::StubSetup& setup)
{
    ctx.openCase("lockTimeout");

    const cmed::harness::StubDaemon silent{area.shared(), cmed::harness::answerNoLock()};
    cmed::CmedSession session = setup.openRequester(shortDeadlines());

    bool timedOut = false;
    const timing::Stopwatch waited;
    try
    {
        // The cast is what the call is for: this line is expected to throw, not to return a guard.
        static_cast<void>(session.lock(DomainName));
    }
    catch (const cmed::CmedLockTimeoutError&)
    {
        timedOut = true;
    }
    const auto elapsed = waited.elapsed();

    ctx.check(timedOut, "lock() throws CmedLockTimeoutError");
    ctx.check(elapsed >= ShortWait, "it waited at least the configured deadline");
    ctx.checkf(elapsed < Ceiling, "and gave up in %" PRId64 " ms, under the %" PRId64 " ms a leaked default would pass",
               static_cast<std::int64_t>(
                   timing::getTicks<timing::Millis>(elapsed)),
               static_cast<std::int64_t>(Ceiling.count()));
}

// A release carries no deadline: it waits for nobody, since the holder publishes Idle and rings
// on its own. A daemon that is not there is what tells that apart from waiting on the turn.
void theReleaseWaitsForNobody(probe::Context& ctx, cmed::harness::ProbeArea& area,
                              const cmed::harness::StubSetup& setup)
{
    ctx.openCase("release with nobody to answer");

    cmed::CmedSession session = setup.openRequester(shortDeadlines());

    std::optional<cmed::CmedGuard> guard;
    {
        const cmed::harness::StubDaemon granting{area.shared()};
        guard = session.tryLock(DomainName, timing::Secs{2});
    }

    // The daemon is gone before the release, so anything this call waited for would never arrive.
    if (!ctx.check(guard.has_value(), "the domain was granted while a daemon was running"))
    {
        return;
    }

    const timing::Stopwatch waited;
    guard.reset();
    const auto elapsed = waited.elapsed();

    ctx.checkf(elapsed < ShortWait, "the release returns without waiting, in %" PRId64 " ms",
               static_cast<std::int64_t>(timing::getTicks<timing::Millis>(elapsed)));
    ctx.check(cmed::harness::isIdle(area.shared(), DomainSlot),
              "and the domain is left at Idle for the next requester");
}

// Which socket connect() reaches for is the config's to say. An unbound path is what makes a wrong
// socketPath visible, and one attempt is the whole of it: the answer says a later call may find one.
void theSocketPathReachesConnect(probe::Context& ctx)
{
    ctx.openCase("socketPath");

    cmed::CmedClientConfig_t config = shortDeadlines();
    config.socketPath = UnboundSocket;

    bool refused = false;
    const timing::Stopwatch waited;
    try
    {
        // The cast is what the call is for: this line is expected to throw, not to return a session.
        static_cast<void>(cmed::CmedSession::connect(config));
    }
    catch (const cmed::CmedAreaNotReadyError&)
    {
        refused = true;
    }
    const auto elapsed = waited.elapsed();

    ctx.check(refused, "connect throws CmedAreaNotReadyError");
    ctx.checkf(elapsed < ShortWait, "and it does not wait, at %" PRId64 " ms",
               static_cast<std::int64_t>(timing::getTicks<timing::Millis>(elapsed)));
}

// A socket bound with nobody accepting on it. The connection lands in the backlog and costs nothing,
// so what the deadline below bounds is the wait for a Welcome that never comes and nothing else.
void theAttachTimeoutReachesTheWelcomeWait(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("setupTimeout");

    const std::string path = scratch.makePath("silent.sock");
    const auto listening = posix::SeqpacketSocket::listen(path, SocketMode, Backlog);

    cmed::CmedClientConfig_t config = shortDeadlines();
    config.socketPath = path;

    bool refused = false;
    const timing::Stopwatch waited;
    try
    {
        // The cast is what the call is for: this line is expected to throw, not to return a session.
        static_cast<void>(cmed::CmedSession::connect(config));
    }
    catch (const cmed::CmedAreaNotReadyError&)
    {
        refused = true;
    }
    const auto elapsed = waited.elapsed();

    ctx.check(refused, "connect throws CmedAreaNotReadyError");
    ctx.check(elapsed >= ShortWait, "it waited out the configured deadline for the Welcome");
    ctx.checkf(elapsed < Ceiling, "and gave up in %" PRId64 " ms, under the %" PRId64 " ms a leaked default would pass",
               static_cast<std::int64_t>(
                   timing::getTicks<timing::Millis>(elapsed)),
               static_cast<std::int64_t>(Ceiling.count()));
}

// Which failures connect() waits out and which it reports now. A daemon still starting refuses the
// connection or answers ENOENT, and waiting is right for both. Anything else is this deployment being
// wrong about the socket, and waiting out a deadline before saying so misreports it as a slow start.
void aFailureWaitingCannotMendIsReportedNow(probe::Context& ctx, const cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a socket path waiting cannot mend");

    // A regular file standing where a directory would have to be. connect() answers ENOTDIR, which is
    // not one of the three a daemon still starting produces.
    const std::string blocker = scratch.makePath("not-a-directory");
    {
        std::ofstream standing{blocker};
        standing << "x";
    }

    cmed::CmedClientConfig_t config = shortDeadlines();
    config.socketPath = blocker + "/cmed.sock";

    bool reported = false;
    const timing::Stopwatch waited;
    try
    {
        // The cast is what the call is for: this line is expected to throw, not to return a session.
        static_cast<void>(cmed::CmedSession::connect(config));
    }
    catch (const cmed::CmedBackendError&)
    {
        reported = true;
    }
    catch (const cmed::CmedAreaNotReadyError&)
    {
        reported = false;
    }
    const auto elapsed = waited.elapsed();

    ctx.check(reported, "connect throws CmedBackendError rather than the not-ready this deadline would produce");
    ctx.checkf(elapsed < ShortWait, "and reports it in %" PRId64 " ms, without waiting out the deadline",
               static_cast<std::int64_t>(timing::getTicks<timing::Millis>(elapsed)));
}

// A path sun_path cannot hold. Refused rather than truncated, since a truncated address names
// somewhere else, and the refusal is the one that leaves the library's own error hierarchy.
void aSocketPathThatDoesNotFit(probe::Context& ctx)
{
    ctx.openCase("a socket path the address cannot hold");

    cmed::CmedClientConfig_t config = shortDeadlines();
    config.socketPath = "./" + std::string(200, 'x') + ".sock";

    bool refused = false;
    const timing::Stopwatch waited;
    try
    {
        // The cast is what the call is for: this line is expected to throw, not to return a session.
        static_cast<void>(cmed::CmedSession::connect(config));
    }
    // Two catches and not one for std::exception, so a refusal arriving as anything but one of the two
    // layers a caller of this library writes a handler for still reads as no refusal at all.
    catch (const cmed::CmedError&)
    {
        refused = true;
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    const auto elapsed = waited.elapsed();

    ctx.check(refused, "connect refuses it rather than answering with a session");
    ctx.checkf(elapsed < ShortWait, "and it does not wait, at %" PRId64 " ms",
               static_cast<std::int64_t>(timing::getTicks<timing::Millis>(elapsed)));
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"session-config-probe"};
    return probe::run("session config probe",
                      [&scratch](probe::Context& ctx)
                      {
                          const std::string areaLabel = scratch.makeAreaName("");
                          cmed::harness::ProbeArea area{areaLabel.c_str()};
                          cmed::harness::publishDomain(area.shared(), DomainSlot, DomainName);
                          const cmed::harness::StubSetup setup{scratch.makePath("cmed.sock"),
                                                               area.descriptor()};

                          theLockTimeoutReachesLock(ctx, area, setup);
                          theReleaseWaitsForNobody(ctx, area, setup);
                          theSocketPathReachesConnect(ctx);
                          theAttachTimeoutReachesTheWelcomeWait(ctx, scratch);
                          aFailureWaitingCannotMendIsReportedNow(ctx, scratch);
                          aSocketPathThatDoesNotFit(ctx);
                      });
}
