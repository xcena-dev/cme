// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/control/handler.cpp -- see handler.hpp.

#include "daemon/control/handler.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/timing.hpp"
#include "daemon/control/exchange.hpp"
#include "daemon/control/listener.hpp"
#include "daemon/domain/manager.hpp"
#include "daemon/observe/counters.hpp"
#include "daemon/observe/failpoint.hpp"
#include "daemon/startup/config.hpp"
#include "shared/posix/epoll.hpp"
#include "shared/posix/event_fd.hpp"
#include "shared/posix/seqpacket_socket.hpp"
#include "shared/posix/unique_fd.hpp"
#include "shared/protocol/message.hpp"

namespace cmed::daemon
{

// ── file-local ─────────────────────────────────────────────────────────

namespace
{

// One pass handles at most this many ready descriptors before looking again. A bound rather than a
// buffer that grows: the loop has other work at the top, and a burst should not delay reaching it.
constexpr std::uint32_t ReadyAtOnce = 32;

// Which bits of posix::EpollFlag a connection is registered with, and which of them say the peer is
// gone. Its own rather than the wrapper's, since choosing among those bits is this loop's policy.
struct ConnectionEvents
{
    // A connection asks for both: a message to read, and being told when the peer goes.
    static constexpr posix::EventMask Watched = posix::EpollFlag::Readable | posix::EpollFlag::PeerGone;

    // The three ways a wake-up says the peer is gone. Never registered: the kernel reports HungUp and
    // Failed whether or not a caller asked for them.
    static constexpr posix::EventMask Gone =
        posix::EpollFlag::PeerGone | posix::EpollFlag::HungUp | posix::EpollFlag::Failed;
};

}  // namespace

// ── what the loop holds ────────────────────────────────────────────────

// A connection and how far it has got. Greeting twice would hand out a second descriptor for one
// request, so the flag is what makes the second Hello answerable at all.
struct Connection_t
{
    posix::SeqpacketSocket socket;
    bool greeted{false};

    // The version this peer claimed, which every answer to it is stamped with. Zero until the first
    // message, since a connection that has said nothing has claimed nothing.
    std::uint32_t claimedVersion{0};

    // What this peer joined and has not left, so a departure knows what to give back.
    JoinLedger joins;
};

// One entry per admitted requester, holding that requester's own socket and its own state. Keyed by
// descriptor, which is also the token the epoll carries back.
struct Peers_t
{
    std::unordered_map<posix::FileDesc, Connection_t> connected;
};

// ── ctor / dtor ────────────────────────────────────────────────────────

ControlHandler::ControlHandler(const DaemonConfig_t& config, posix::FileDesc areaDescriptor,
                               std::uint32_t areaBytes, DomainManager& domains)
    : polling_{{config.socketPath(), config.socket.mode, config.admit},
               posix::Epoll::create(ReadyAtOnce),
               posix::EventFd::create()},
      answers_{areaDescriptor, HelloExchange{areaBytes}, DomainRequestExchange{domains}},
      peers_{std::make_unique<Peers_t>()},
      state_{true, config.control.idleInterval, config.control.maxConnections}
{
    // The descriptor is its own token throughout, so a ready entry needs no table to be recognised.
    polling_.watching.watch(polling_.admitting.descriptor(), posix::EpollFlag::Readable);
    polling_.watching.watch(polling_.stopSignal.descriptor(), posix::EpollFlag::Readable);
}

ControlHandler::~ControlHandler() noexcept = default;

// ── the loop, and when it ends ─────────────────────────────────────────

void ControlHandler::run()
{
    while (state_.running.load(std::memory_order_acquire))
    {
        for (const posix::Epoll::Ready_t entry : polling_.watching.wait(state_.idleInterval))
        {
            const posix::FileDesc woke = entry.descriptor;
            const posix::EventMask events = entry.events;

            if (woke == polling_.stopSignal.descriptor())
            {
                static_cast<void>(polling_.stopSignal.drain());
                state_.running.store(false, std::memory_order_release);
                continue;
            }
            if (woke == polling_.admitting.descriptor())
            {
                admitConnections();
                continue;
            }

            // The message first. A peer that sent its Hello and shut the connection down in the same
            // breath is answerable, and judging the hangup first would drop what it already said.
            if (posix::hasEvent(events, posix::EpollFlag::Readable))
            {
                readMessage(woke);
            }
            if (posix::hasEvent(events, ConnectionEvents::Gone))
            {
                dropConnection(woke);
            }
        }
    }
}

void ControlHandler::stop() const noexcept
{
    static_cast<void>(polling_.stopSignal.post());
}

// ── who comes and goes ─────────────────────────────────────────────────

// Until the queue is empty, not once per wake. The epoll reports the listener ready, not how many are
// queued behind it, and one accept per pass would leave the rest waiting on the next event.
void ControlHandler::admitConnections()
{
    while (true)
    {
        auto admitted = polling_.admitting.take();

        // No default: a judgement this switch does not name is a compiler error here, rather than a
        // connection registered as though it had been admitted.
        switch (admitted.outcome)
        {
            case Listener::Admission::Again:
                return;
            case Listener::Admission::Refused:
                continue;
            case Listener::Admission::Accepted:
                break;
        }

        // Taken from the queue before it is judged, so a refusal closes its descriptor here rather
        // than leaving the arrival to be accepted again on the next pass.
        if (peers_->connected.size() >= state_.maxConnections)
        {
            observe_.events.bump(observe::ControlEvent::Crowded);
            continue;
        }

        const auto descriptor = admitted.socket.get();
        polling_.watching.watch(descriptor, ConnectionEvents::Watched);
        peers_->connected.emplace(descriptor, Connection_t{posix::SeqpacketSocket::adopt(std::move(admitted.socket)), false});
        observe_.events.bump(observe::ControlEvent::Admitted);
    }
}

// Closing removes it from the epoll, so the erase is the whole of it. A descriptor arriving here twice
// is not an error: the second pass finds nothing to erase and says so by doing nothing.
void ControlHandler::dropConnection(posix::FileDesc descriptor)
{
    const auto found = peers_->connected.find(descriptor);
    if (found == peers_->connected.end())
    {
        return;
    }

    // A peer that exited tidily left first and has nothing here. One that died did not, and this is
    // the only signal it leaves: unread, the node stays in a domain no other node may then delete.
    failpoint::reach(failpoint::Boundary::DepartBeforeGiveBack);
    answers_.domainRequests.giveBack(found->second.joins);

    peers_->connected.erase(found);
    observe_.events.bump(observe::ControlEvent::Departed);
}

// ── one message ────────────────────────────────────────────────────────

namespace
{

// What one receive turned out to be. A message means the whole of one arrived; empty with unfollowable
// set is a peer that said what no version of this protocol sends, and empty without it a peer that closed.
struct Heard_t
{
    std::optional<posix::SeqpacketSocket::Received_t> received;
    bool unfollowable{false};

    // Whether the peer said anything at all. A close is the one outcome that is nobody's fault.
    [[nodiscard]] bool spoke() const noexcept
    {
        return received.has_value() || unfollowable;
    }
};

[[nodiscard]] Heard_t receiveOne(posix::SeqpacketSocket& from)
{
    try
    {
        return {from.receive(protocol::LongestMessage, protocol::MostDescriptors), false};
    }
    catch (const std::system_error&)
    {
        // A message longer than this daemon is prepared for, which no version of the protocol sends.
        return {std::nullopt, true};
    }
}

// Whether the answer went out. Nothing to send is a refusal and not a silence: an exchange that will
// not answer has already decided the connection is to be dropped.
[[nodiscard]] bool sendBack(posix::SeqpacketSocket& asking, const std::optional<std::string>& answered,
                            const std::vector<posix::FileDesc>& carried)
{
    return answered && asking.send(*answered, carried);
}

}  // namespace

// One message per ready event. A SEQPACKET receive takes one whole message, so a second one waiting
// leaves the descriptor readable and the epoll reports it again on the next pass.
void ControlHandler::readMessage(posix::FileDesc descriptor)
{
    const auto found = peers_->connected.find(descriptor);
    if (found == peers_->connected.end())
    {
        return;
    }

    const Heard_t heard = receiveOne(found->second.socket);
    if (heard.received && answer(found->second, heard.received->message))
    {
        return;
    }

    // A peer that closed left nothing to count against it. Anything else that reaches here spoke and
    // was not followed.
    if (heard.spoke())
    {
        observe_.events.bump(observe::ControlEvent::Misspoken);
    }
    dropConnection(descriptor);
}

bool ControlHandler::answer(Connection_t& asking, const std::string& message)
{
    // The base reads the header and accepts whatever type arrived, which is what lets this pick an
    // exchange before any of them has claimed the bytes.
    const protocol::Message arrived{message};
    if (!arrived.isReadable())
    {
        return false;
    }

    // Recorded before the first answer goes out, since that answer has to be stamped with it. A peer
    // that changes its claim mid-connection is speaking a protocol this one does not.
    if (asking.claimedVersion != 0 && asking.claimedVersion != arrived.version())
    {
        return false;
    }
    asking.claimedVersion = arrived.version();

    // A greeting is answered once, and a request only after one: a second Hello would hand out a
    // second descriptor, and a create before any would answer a peer that may yet be refused.
    if (answers_.greeting.answers(arrived.type()))
    {
        if (asking.greeted)
        {
            return false;
        }

        failpoint::reach(failpoint::Boundary::WelcomeBeforeAnswer);
        if (!sendBack(asking.socket, answers_.greeting.answer(arrived, message, asking.joins),
                      {answers_.areaDescriptor}))
        {
            return false;
        }

        asking.greeted = true;
        observe_.events.bump(observe::ControlEvent::Welcomed);
        return true;
    }

    if (answers_.domainRequests.answers(arrived.type()))
    {
        if (!asking.greeted)
        {
            return false;
        }

        if (!sendBack(asking.socket, answers_.domainRequests.answer(arrived, message, asking.joins), {}))
        {
            return false;
        }

        observe_.events.bump(observe::ControlEvent::Answered);
        return true;
    }

    // An unknown type is counted and discarded rather than fatal. One message this build does not
    // know must not end a connection that is otherwise speaking the protocol correctly.
    observe_.events.bump(observe::ControlEvent::Misspoken);
    return true;
}

}  // namespace cmed::daemon
