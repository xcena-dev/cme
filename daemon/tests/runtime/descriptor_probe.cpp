// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// descriptor_probe.cpp -- the three descriptors the daemon's loop and its startup are built out of.
//
// mapping_probe.cpp does this for the two owners of a mapping, and these carry the same bug shape: a
// close that must happen once, and a move that must leave the source owning nothing. The wait is here
// too, because the counter's whole purpose is being waited on rather than polled, and every wait is
// bounded in tens of milliseconds so a broken registration is a red probe and not a hung one.

#include <unistd.h>

#include <cinttypes>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "common/timing.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/posix/epoll.hpp"
#include "shared/posix/event_fd.hpp"
#include "shared/posix/lock_file.hpp"
#include "tests/probe_context.hpp"

namespace
{

// Distinct bits, so a case can OR what came back and still tell which entries it saw.
constexpr std::uint64_t StopToken = 1;
constexpr std::uint64_t DoorbellToken = 2;

// Long enough that a post already made is certainly seen, and short enough to pay in a suite.
constexpr timing::Millis Patience{500};

// The limit a wait nothing answers is measured against.
constexpr timing::Millis WaitLimit{20};

// Set from main(), inside this run's own directory so two probes at once take different locks.
std::string g_lockPath;
std::string g_unopenablePath;

template <typename T_Body>
[[nodiscard]] bool throwsSystemError(T_Body body)
{
    try
    {
        body();
    }
    catch (const std::system_error&)
    {
        return true;
    }
    return false;
}

void aCounterIsTakenBackToZeroAndNotByOne(probe::Context& ctx)
{
    ctx.openCase("the counter a stop arrives on");

    const auto waking = posix::EventFd::create();
    ctx.check(static_cast<bool>(waking), "a created counter holds a descriptor");
    ctx.check(!waking.drain(), "and a fresh one has nothing to take back");

    ctx.check(waking.post(), "a post is written whole");
    ctx.check(waking.drain(), "the drain finds it");
    ctx.check(!waking.drain(), "and a second drain finds nothing");

    // Two posts and one drain, because the counter is read as one wakeup and not as a queue: a loop
    // that had to drain per post would be woken again for a stop it already handled.
    ctx.check(waking.post() && waking.post(), "two posts land");
    ctx.check(waking.drain(), "one drain takes both");
    ctx.check(!waking.drain(), "leaving nothing behind");
}

void aCounterHandedOnLeavesTheSourceEmpty(probe::Context& ctx)
{
    ctx.openCase("a counter handed on");

    auto source = posix::EventFd::create();
    const posix::EventFd taken{std::move(source)};

    // Reading the moved-from counter is the assertion, so the analyser is told once for the group
    // rather than once per line.
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    ctx.check(!static_cast<bool>(source), "the source owns nothing after the move");
    ctx.check(source.descriptor() < 0, "so its destructor has no descriptor to close a second time");
    ctx.check(!source.post(), "a post through it is reported failed rather than thrown");
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    ctx.check(taken.post() && taken.drain(), "while the one that took it posts and drains");

    const posix::EventFd empty;
    ctx.check(!static_cast<bool>(empty), "and a counter that was never created owns nothing either");
}

void aWatchedCounterComesBackWithItsToken(probe::Context& ctx)
{
    ctx.openCase("one wait over one counter");

    auto waiting = posix::Epoll::create(4);
    const auto waking = posix::EventFd::create();
    waiting.watch(waking.descriptor(), posix::EpollFlag::Readable);

    ctx.check(waiting.wait(WaitLimit).size() == 0, "nothing is ready before a post");

    ctx.check(waking.post(), "the counter is posted");
    const posix::Epoll::Pass pass = waiting.wait(Patience);
    if (!ctx.check(pass.size() == 1, "and exactly one entry comes back ready"))
    {
        return;
    }

    const posix::Epoll::Ready_t entry = *pass.begin();
    ctx.checkf(entry.descriptor == waking.descriptor(), "the entry names the descriptor it registered (%d)",
               entry.descriptor);
    ctx.check((entry.events & posix::EpollFlag::Readable) != 0, "and says that descriptor is readable");
}

void aWaitNothingAnswersEndsAtItsLimit(probe::Context& ctx)
{
    ctx.openCase("a wait nothing answers");

    auto waiting = posix::Epoll::create(2);
    const auto waking = posix::EventFd::create();
    waiting.watch(waking.descriptor(), posix::EpollFlag::Readable);

    const timing::Stopwatch spent;
    const std::uint32_t seen = waiting.wait(WaitLimit).size();
    const auto took = spent.elapsed<timing::Millis>();

    ctx.check(seen == 0, "a limit reached with nothing ready answers zero rather than throwing");
    ctx.checkf(took >= WaitLimit, "and the wait was parked for its whole limit (%" PRId64 " ms of %" PRId64 ")",
               static_cast<std::int64_t>(took.count()), static_cast<std::int64_t>(WaitLimit.count()));
}

void oneWaitCoversBothAndTheVectorCapsOnePass(probe::Context& ctx)
{
    ctx.openCase("two counters, one wait");

    auto waiting = posix::Epoll::create(4);
    const auto stopping = posix::EventFd::create();
    const auto knocking = posix::EventFd::create();
    waiting.watch(stopping.descriptor(), posix::EpollFlag::Readable);
    waiting.watch(knocking.descriptor(), posix::EpollFlag::Readable);
    ctx.check(stopping.post() && knocking.post(), "both counters are posted");

    const posix::Epoll::Pass both = waiting.wait(Patience);
    ctx.check(both.size() == 2, "one wait answers for both, so neither is blocked on the other");

    bool sawStopping = false;
    bool sawKnocking = false;
    for (const posix::Epoll::Ready_t named : both)
    {
        sawStopping = sawStopping || named.descriptor == stopping.descriptor();
        sawKnocking = sawKnocking || named.descriptor == knocking.descriptor();
    }
    ctx.check(sawStopping && sawKnocking, "and each entry names its own descriptor");

    // The room one pass has belongs to the Epoll, so a loop cannot be handed more work at once than it
    // was built for. What is left over is still ready on the pass after it.
    auto narrow = posix::Epoll::create(1);
    narrow.watch(stopping.descriptor(), posix::EpollFlag::Readable);
    narrow.watch(knocking.descriptor(), posix::EpollFlag::Readable);
    ctx.check(narrow.wait(Patience).size() == 1, "an epoll built for one answers for one");
    ctx.check(narrow.wait(Patience).size() == 1, "and the other is still ready on the next pass");
}

void watchRefusesWhatItCannotRegister(probe::Context& ctx)
{
    ctx.openCase("a descriptor epoll will not take");

    auto waiting = posix::Epoll::create(2);
    const auto waking = posix::EventFd::create();
    waiting.watch(waking.descriptor(), posix::EpollFlag::Readable);

    ctx.check(throwsSystemError([&waiting, &waking]
                                {
                                    waiting.watch(waking.descriptor(), posix::EpollFlag::Readable);
                                }),
              "the same descriptor twice is refused rather than registered twice");
    ctx.check(throwsSystemError([&waiting]
                                {
                                    waiting.watch(-1, posix::EpollFlag::Readable);
                                }),
              "and a descriptor that is not one at all");
}

void oneHolderAtATimeUnderOneName(probe::Context& ctx)
{
    ctx.openCase("the file whose flock says one daemon runs");

    {
        const std::optional<posix::LockFile> held = posix::LockFile::take(g_lockPath);
        ctx.check(held.has_value(), "the first caller takes the lock");

        // Two open descriptors on one file are arbitrated separately, so this is refused inside one
        // process exactly as it is across two.
        ctx.check(!posix::LockFile::take(g_lockPath).has_value(), "and a second is told the name is busy");
    }

    ctx.check(posix::LockFile::take(g_lockPath).has_value(), "the lock is free once its holder goes out of scope");
    ctx.check(::access(g_lockPath.c_str(), F_OK) == 0, "while the file stays, so a starter has a path to open");
}

void aLockHandedOnStaysHeld(probe::Context& ctx)
{
    ctx.openCase("a lock handed on");

    std::optional<posix::LockFile> held = posix::LockFile::take(g_lockPath);
    ctx.check(held.has_value(), "the lock is taken");
    if (!held.has_value())
    {
        return;
    }

    {
        const posix::LockFile taken{std::move(*held)};

        // NOLINTNEXTLINE(bugprone-use-after-move): reading the moved-from lock is the assertion.
        ctx.check(held->descriptor() < 0, "the source holds no descriptor after the move");

        held.reset();
        ctx.check(!posix::LockFile::take(g_lockPath).has_value(), "and dropping the emptied source releases nothing");
        ctx.check(taken.descriptor() >= 0, "the one that took it holds the descriptor");
    }

    ctx.check(posix::LockFile::take(g_lockPath).has_value(), "which is what releases the lock when it goes");
}

void aPathThatCannotBeOpenedIsAFaultAndNotABusyName(probe::Context& ctx)
{
    ctx.openCase("a lock path that cannot be opened");

    // Busy is an answer a starter acts on, and an unopenable path is a deployment fault. A caller
    // reading the second as the first would report a daemon already running.
    ctx.check(throwsSystemError([]
                                {
                                    static_cast<void>(posix::LockFile::take(g_unopenablePath));
                                }),
              "a path whose directory is absent throws rather than answering busy");
}

}  // namespace

int main()
{
    const cmed::harness::ProbeScratch scratch{"descriptor-probe"};
    g_lockPath = scratch.makePath("cmed.lock");
    g_unopenablePath = scratch.makePath("absent-directory/cmed.lock");

    return probe::run("descriptor probe",
                      [](probe::Context& ctx)
                      {
                          aCounterIsTakenBackToZeroAndNotByOne(ctx);
                          aCounterHandedOnLeavesTheSourceEmpty(ctx);
                          aWatchedCounterComesBackWithItsToken(ctx);
                          aWaitNothingAnswersEndsAtItsLimit(ctx);
                          oneWaitCoversBothAndTheVectorCapsOnePass(ctx);
                          watchRefusesWhatItCannotRegister(ctx);
                          oneHolderAtATimeUnderOneName(ctx);
                          aLockHandedOnStaysHeld(ctx);
                          aPathThatCannotBeOpenedIsAFaultAndNotABusyName(ctx);
                      });
}
