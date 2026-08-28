// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// seqpacket_probe.cpp -- the receiving side, at the edges a message the daemon sends never reaches.
//
// fdpass_probe covers a descriptor crossing and a peer that closed. What it does not cover is what
// the receive does when the message does not fit: the buffer, the descriptor count, or the deadline.
//
// A truncated control buffer is how a descriptor goes missing with neither end seeing an error, so
// the refusal is the whole contract. Every case here reads it as a throw rather than as short bytes.

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "common/timing.hpp"
#include "harness/helper_scratch.hpp"
#include "harness/helper_socket.hpp"
#include "shared/posix/mem_file.hpp"
#include "shared/posix/seqpacket_socket.hpp"
#include "shared/posix/unique_fd.hpp"
#include "tests/probe_context.hpp"

namespace
{

constexpr ::mode_t SocketMode = 0600;

// This probe's own ceiling, well under the protocol's, so a case can send one byte past it without
// building a message any real reader would take.
constexpr std::uint32_t MostBytes = 64;

constexpr std::uint64_t FileBytes = 4096;

// Long enough that a scheduler hiccup cannot end it early, short enough to keep the probe quick.
constexpr timing::Millis WaitFor{80};

// What a wait that returned on its deadline may not exceed. A receive that ignored the deadline
// entirely would block until the case timed the whole probe out rather than land here.
constexpr timing::Millis Ceiling{2000};

[[nodiscard]] posix::MemFile someFile()
{
    return posix::MemFile::create("cmed-seqpacket-probe", FileBytes);
}

// How many descriptors this process holds. A refusal has to close what already arrived, and the only
// way to see that from outside is to count them before and after.
[[nodiscard]] std::uint32_t countOpenDescriptors()
{
    std::uint32_t open = 0;
    std::error_code failed;
    for (const auto& entry : std::filesystem::directory_iterator{"/proc/self/fd", failed})
    {
        static_cast<void>(entry);
        ++open;
    }
    return open;
}

// The buffer is the caller's contract, so a message past it is refused rather than handed over as its
// leading bytes. A control message read half way is one whose header says something else.
void aMessagePastTheBufferIsRefused(probe::Context& ctx, const std::string& path)
{
    ctx.openCase("a message longer than the buffer");

    cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(path, SocketMode);

    const std::string exact(MostBytes, 'a');
    ctx.check(pair.serving.send(exact, {}), "a message of exactly the buffer's size is sent");
    const std::optional<posix::SeqpacketSocket::Received_t> fitted = pair.asking.receive(MostBytes, 0);
    ctx.check(fitted && fitted->message == exact, "and arrives whole, so the ceiling is inclusive");

    const std::string past(MostBytes + 1, 'b');
    ctx.check(pair.serving.send(past, {}), "one byte past it is sent too");

    bool refused = false;
    try
    {
        static_cast<void>(pair.asking.receive(MostBytes, 0));
    }
    catch (const std::system_error& failure)
    {
        refused = failure.code().value() == EMSGSIZE;
    }
    ctx.check(refused, "and is refused with EMSGSIZE rather than handed over as its first bytes");

    // The connection survives the refusal. A receive that left the socket unusable would turn one
    // oversized message into the end of a peer that is otherwise speaking correctly.
    ctx.check(pair.serving.send("after", {}), "the next message is sent on the same connection");
    const std::optional<posix::SeqpacketSocket::Received_t> next = pair.asking.receive(MostBytes, 0);
    ctx.check(next && next->message == "after", "and arrives whole after the refusal");
}

// The refusal has to come after the adopt, or the ones that did arrive are descriptors leaked into a
// process that never asked for them. That half is what the descriptor counts below read.
[[nodiscard]] bool refusesTheMessage(posix::SeqpacketSocket& asking, std::uint32_t mostDescriptors)
{
    try
    {
        static_cast<void>(asking.receive(MostBytes, mostDescriptors));
    }
    catch (const std::system_error& failure)
    {
        return failure.code().value() == EMSGSIZE;
    }
    return false;
}

// A count one over what the receiver asked for may still fit its control buffer, since CMSG_SPACE
// rounds to eight bytes. So the answer that is read here is the leak and not the acceptance.
void moreDescriptorsThanTheReceiverAskedFor(probe::Context& ctx, const std::string& path)
{
    ctx.openCase("more descriptors than were asked for");

    const posix::MemFile first = someFile();
    const posix::MemFile second = someFile();
    const posix::MemFile third = someFile();

    {
        cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(path, SocketMode);
        ctx.check(pair.serving.send("one", {first.descriptor()}), "one descriptor is sent");
        ctx.check(refusesTheMessage(pair.asking, 0), "a receiver expecting none refuses it with EMSGSIZE");
    }

    {
        cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(path, SocketMode);
        ctx.check(pair.serving.send("two", {first.descriptor(), second.descriptor()}), "two are sent");

        const std::uint32_t before = countOpenDescriptors();
        const bool refused = refusesTheMessage(pair.asking, 1);
        const std::uint32_t after = countOpenDescriptors();

        ctx.checkf(after == before, "two against one asked is %s and holds none: %" PRIu32 " open before, %" PRIu32 " after",
                   refused ? "refused" : "taken", before, after);
    }

    {
        cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(path, SocketMode);
        ctx.check(pair.serving.send("three", {first.descriptor(), second.descriptor(), third.descriptor()}),
                  "three are sent");

        const std::uint32_t before = countOpenDescriptors();
        const bool refused = refusesTheMessage(pair.asking, 1);
        const std::uint32_t after = countOpenDescriptors();

        ctx.check(refused, "and three against one asked is refused, which is where the next bucket starts");
        ctx.checkf(after == before, "the refusal closing what arrived: %" PRIu32 " open before, %" PRIu32 " after",
                   before, after);
    }
}

// Far enough above any control buffer that fits on a stack frame that a scan reaching it means the
// socket has no ceiling at all.
constexpr std::uint32_t ScanBound = 64;

// The lowest descriptor count @serving refuses, or zero when none up to ScanBound is refused. The
// ceiling is private to the socket, so it is found rather than named and follows the constant.
[[nodiscard]] std::uint32_t findFirstRefusedCount(const posix::SeqpacketSocket& serving, posix::FileDesc file)
{
    for (std::uint32_t count = 1; count <= ScanBound; ++count)
    {
        const std::vector<posix::FileDesc> attached(count, file);
        try
        {
            static_cast<void>(serving.send("scan", attached));
        }
        catch (const std::invalid_argument&)
        {
            return count;
        }
    }
    return 0;
}

// A ceiling rather than a tunable, and one ceiling for both directions: a send attaching past it and a
// receive asking past it are refused, so neither end can name a count the other would not carry.
void aDescriptorCountPastTheCeiling(probe::Context& ctx, const std::string& path)
{
    ctx.openCase("a descriptor count no message may carry");

    cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(path, SocketMode);
    const posix::MemFile file = someFile();

    const std::uint32_t pastCeiling = findFirstRefusedCount(pair.serving, file.descriptor());
    ctx.checkf(pastCeiling != 0, "a send refuses some count at or under %" PRIu32 ", at %" PRIu32, ScanBound,
               pastCeiling);
    if (pastCeiling == 0)
    {
        return;
    }

    bool receiveRefused = false;
    try
    {
        static_cast<void>(pair.asking.receive(MostBytes, pastCeiling, WaitFor));
    }
    catch (const std::invalid_argument&)
    {
        receiveRefused = true;
    }
    ctx.check(receiveRefused, "and a receive asking for that same count is refused too");

    bool belowRefused = false;
    try
    {
        static_cast<void>(pair.asking.receive(MostBytes, pastCeiling - 1, WaitFor));
    }
    catch (const std::invalid_argument&)
    {
        belowRefused = true;
    }
    ctx.checkf(!belowRefused, "while the count below it, %" PRIu32 ", is one both ends carry", pastCeiling - 1);
}

// The deadline is set every call, so nothing carries over. A receive that kept the last call's value
// would block a control loop that meant to look once and go back to its other work.
void theDeadlineIsThisCallsAlone(probe::Context& ctx, const std::string& path)
{
    ctx.openCase("the deadline one receive was given");

    cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(path, SocketMode);

    const timing::Stopwatch waited;
    const std::optional<posix::SeqpacketSocket::Received_t> nothing = pair.asking.receive(MostBytes, 0, WaitFor);
    const auto elapsed = waited.elapsed();

    ctx.check(!nothing, "a receive with nobody sending answers nothing");
    ctx.checkf(elapsed >= WaitFor, "after waiting its whole deadline, %" PRId64 " ms",
               static_cast<std::int64_t>(timing::getTicks<timing::Millis>(elapsed)));
    ctx.checkf(elapsed < Ceiling, "and no longer than that, under the %" PRId64 " ms a lost deadline would pass",
               static_cast<std::int64_t>(Ceiling.count()));

    // The next call names no deadline, which is what a caller epoll already told there is a message
    // to read passes. A deadline left over from the call above would still be in force here.
    ctx.check(pair.serving.send("queued", {}), "a message is queued for the next call");
    const std::optional<posix::SeqpacketSocket::Received_t> queued = pair.asking.receive(MostBytes, 0);
    ctx.check(queued && queued->message == "queued", "a receive naming no deadline takes what is waiting");
}

// Stamped by the kernel at connect time rather than asked for, so a recycled pid cannot be the one
// answering. The refusal is nothing, because a socket that cannot say is not one to guess about.
void whoTheKernelSaysConnected(probe::Context& ctx, const std::string& path)
{
    ctx.openCase("the credentials the kernel stamped");

    const cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(path, SocketMode);

    const std::optional<::ucred> peer = posix::readPeer(pair.serving.descriptor());
    // Recorded and then tested rather than tested through check's return, since a reader of the
    // accesses below cannot tell that a call decided whether they run.
    ctx.check(peer.has_value(), "a connected socket answers for its peer");
    if (!peer.has_value())
    {
        return;
    }
    ctx.check(peer->pid == ::getpid() && peer->uid == ::geteuid() && peer->gid == ::getegid(),
              "with this process, since both ends are here");

    const posix::MemFile file = someFile();
    ctx.check(!posix::readPeer(file.descriptor()).has_value(),
              "and a descriptor that is not a socket answers nothing rather than zeros");
}

// A peer that closed and a peer that sent nothing read the same, because recvmsg answers zero for
// both. That is the one place a caller cannot tell an empty message from a departure.
void anEmptyMessageReadsAsADeparture(probe::Context& ctx, const std::string& path)
{
    ctx.openCase("a message carrying no bytes at all");

    cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(path, SocketMode);

    ctx.check(pair.serving.send("", {}), "a message of no bytes is sent");
    ctx.check(!pair.asking.receive(MostBytes, 0, WaitFor),
              "and reads as nothing, which is also how a peer that left reads");

    // Proved against the departure itself, so the two answers are compared rather than one of them
    // being read as the other by assumption.
    pair.serving = posix::SeqpacketSocket{};
    ctx.check(!pair.asking.receive(MostBytes, 0, WaitFor), "a peer that closed reads the same way");
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"seqpacket-probe"};
    return probe::run("seqpacket probe",
                      [&scratch](probe::Context& ctx)
                      {
                          // One path for every case, rebound each time: the pair's listening socket
                          // removes the name when it goes, so the next case binds it fresh.
                          const std::string path = "./" + scratch.readLabel() + ".sock";
                          static_cast<void>(::unlink(path.c_str()));

                          aMessagePastTheBufferIsRefused(ctx, path);
                          moreDescriptorsThanTheReceiverAskedFor(ctx, path);
                          aDescriptorCountPastTheCeiling(ctx, path);
                          theDeadlineIsThisCallsAlone(ctx, path);
                          whoTheKernelSaysConnected(ctx, path);
                          anEmptyMessageReadsAsADeparture(ctx, path);

                          static_cast<void>(::unlink(path.c_str()));
                      });
}
