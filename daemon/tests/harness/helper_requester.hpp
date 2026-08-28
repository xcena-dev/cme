// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_requester.hpp -- the daemon's half of setup, and a requester that came in through it.
//
// Apart from helper_area.hpp because this is the half that needs the client library. A probe whose
// subject is the layout or the daemon takes the area alone and links neither session nor config.
//
// Setup and the acquire are separate fixtures: this answers the one exchange a requester cannot skip,
// and StubDaemon writes the words of an acquire, which is what a case using both is about.

#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cmed/config.hpp"
#include "cmed/errors.hpp"
#include "cmed/session.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "shared/posix/seqpacket_socket.hpp"
#include "shared/posix/unique_fd.hpp"
#include "shared/protocol/message.hpp"
#include "shared/protocol/messages.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::harness
{

// How long a probe gives a daemon it just started, and the gap between tries. Its own budget and not
// the config's: setupTimeout is the wait for a Welcome on a socket that already accepted.
constexpr timing::Millis StartWait{2000};
constexpr timing::Millis StartStep{2};

// A session on a daemon that may still be starting. connect() makes one attempt and says
// CmedAreaNotReadyError while the socket is not there yet, so the waiting belongs to whoever started it.
[[nodiscard]] inline CmedSession openWhenItAnswers(const CmedClientConfig_t& config,
                                                   timing::Millis within = StartWait)
{
    std::optional<CmedSession> opened = poll::awaitValue(
        [&config]() -> std::optional<CmedSession>
        {
            try
            {
                return CmedSession::connect(config);
            }
            catch (const CmedAreaNotReadyError&)
            {
                return {};
            }
        },
        within, StartStep);

    if (!opened)
    {
        throw CmedAreaNotReadyError{"cmed::harness: no daemon answered on " + config.socketPath};
    }
    return std::move(*opened);
}

// A door onto one area: whoever says Hello here is handed that area's descriptor and keeps the
// connection, which is what a requester's liveness is made of.
class StubSetup
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // @path is bound here and goes with this. @areaDescriptor stays the caller's, since the kernel
    // copies it into each receiver rather than moving it.
    StubSetup(std::string path, posix::FileDesc areaDescriptor)
        : path_{path},
          areaDescriptor_{areaDescriptor},
          listening_{posix::SeqpacketSocket::listen(std::move(path), SocketMode, Backlog)},
          worker_{[this]
                  {
                      run();
                  }}
    {
    }

    StubSetup(const StubSetup&) = delete;
    StubSetup(StubSetup&&) = delete;

    ~StubSetup()
    {
        running_.store(false, std::memory_order_release);
        worker_.join();
    }

    // ── operator= ──────────────────────────────────────────────────
    StubSetup& operator=(const StubSetup&) = delete;
    StubSetup& operator=(StubSetup&&) = delete;

    // ── public methods ─────────────────────────────────────────────
    // Defaulted, since a case not about a deadline wants the compiled-in ones. The path is this
    // door's either way, so it is the one field a caller does not get to name.
    [[nodiscard]] CmedSession openRequester(CmedClientConfig_t config = {}) const
    {
        config.socketPath = path_;
        return CmedSession::connect(config);
    }

    // ── accessors ──────────────────────────────────────────────────
    // What another process reaches this door by. A descriptor it inherited maps the area but greets
    // nobody, and a session is what the greeting produces.
    [[nodiscard]] const std::string& path() const noexcept
    {
        return path_;
    }

private:
    static constexpr ::mode_t SocketMode = 0600;
    static constexpr std::int32_t Backlog = 8;

    // Short rather than blocking: accept() answers nothing when nobody is waiting, so the loop polls
    // and the destructor's flag is read within one turn of being set.
    static constexpr timing::Millis IdleTurn{2};

    // A requester that connects and never greets must not hold the loop, since another one queued
    // behind it would then wait out that silence too.
    static constexpr timing::Millis HelloWait{2000};

    void run()
    {
        while (running_.load(std::memory_order_acquire))
        {
            posix::UniqueFd arrived = listening_.accept();
            if (!arrived)
            {
                std::this_thread::sleep_for(IdleTurn);
                continue;
            }

            auto greeting = posix::SeqpacketSocket::adopt(std::move(arrived));
            if (welcome(greeting))
            {
                // Held rather than dropped: past setup the connection is what says the requester is
                // still there, and a hangup here would read as this daemon having gone instead.
                served_.push_back(std::move(greeting));
            }
        }
    }

    [[nodiscard]] bool welcome(posix::SeqpacketSocket& greeting) const
    {
        const std::optional<posix::SeqpacketSocket::Received_t> asked =
            greeting.receive(protocol::LongestMessage, protocol::MostDescriptors, HelloWait);
        if (!asked)
        {
            return false;
        }

        const protocol::HelloMessage hello{asked->message};
        if (!hello.isReadable())
        {
            return false;
        }

        // The version and the sequence are echoed rather than named: the requester matches the answer
        // against what it stamped, and a peer speaking an older version reads its own.
        return greeting.send(
            protocol::WelcomeMessage::frame(
                hello.version(), hello.sequence(),
                protocol::Welcome_t{protocol::AbiVersion, static_cast<std::uint32_t>(sizeof(protocol::SharedArea_t))}),
            {areaDescriptor_});
    }

    std::string path_;
    posix::FileDesc areaDescriptor_;
    posix::SeqpacketSocket listening_;

    std::atomic<bool> running_{true};

    // Read by nobody: what holding them buys is that they stay open, and the destructor closes them
    // after the thread that fills this has joined.
    std::vector<posix::SeqpacketSocket> served_;

    // Last, so every word above is built before the thread that reads them starts.
    std::thread worker_;
};

}  // namespace cmed::harness
