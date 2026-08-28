// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/control/handler.hpp -- the control half of the daemon: who arrives, what they ask, who leaves,
// and when to stop. Turned on main's own thread.
// Main's own epoll, apart from the domain worker's futex wait, so a grant is never routed through a
// syscall here. Only setup and control travel here -- the area handover plus create/delete/join/leave.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "common/timing.hpp"
#include "daemon/control/exchange.hpp"
#include "daemon/control/listener.hpp"
#include "daemon/domain/manager.hpp"
#include "daemon/observe/counters.hpp"
#include "daemon/startup/config.hpp"
#include "shared/posix/epoll.hpp"
#include "shared/posix/event_fd.hpp"
#include "shared/posix/unique_fd.hpp"

namespace cmed::daemon
{

// Both are defined in the .cpp. What a connection holds and how they are kept are this loop's own
// business, and nothing outside it reaches a connection except through the loop.
struct Connection_t;
struct Peers_t;

class ControlHandler
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // Binds the socket and takes the epoll. Throws when either cannot be had: a daemon with no way in
    // has nothing to serve, so this is a failure to start rather than a degraded run.
    ControlHandler(const DaemonConfig_t& config, posix::FileDesc areaDescriptor, std::uint32_t areaBytes,
                   DomainManager& domains);

    ControlHandler(const ControlHandler&) = delete;
    ControlHandler(ControlHandler&&) = delete;
    // Out of line, because destroying what peers_ holds needs the shape the .cpp gives it.
    ~ControlHandler() noexcept;

    // ── operator= ──────────────────────────────────────────────────────
    ControlHandler& operator=(const ControlHandler&) = delete;
    ControlHandler& operator=(ControlHandler&&) = delete;

    // ── public methods ─────────────────────────────────────────────────
    // Turns on the caller's thread until stop(). One wait is bounded by the config's idle turn, so a
    // loop nobody is connecting to still reaches the top and can be told to leave.
    void run();

    // Ends the loop from another thread or from a signal handler. One write to an eventfd, which is
    // why a handler may call it: a flag would need the loop to wake on its own to notice.
    void stop() const noexcept;

    // ── accessors ──────────────────────────────────────────────────────
    // Read while the loop turns, on whichever thread is measuring it.
    [[nodiscard]] std::uint64_t readCount(observe::ControlEvent asked) const noexcept
    {
        return observe_.events.read(asked);
    }

private:
    // ── private methods ────────────────────────────────────────────────
    // The pair, together: a connection enters the table and the epoll at once, and leaving the table is
    // what takes it out of the epoll.
    void admitConnections();
    void dropConnection(posix::FileDesc descriptor);

    void readMessage(posix::FileDesc descriptor);

    // Whether the connection lives on. False drops it, which is this daemon's only refusal once a
    // message has been read: there is no error message to send that a wrong-version peer could parse.
    [[nodiscard]] bool answer(Connection_t& asking, const std::string& message);

private:
    // ── what one pass waits on ─────────────────────────────────────────
    // The epoll and the two descriptors that are always in it. Every connection this loop accepts joins
    // the same epoll, so what a pass wakes for may be an arrival, a requester, or the stop.
    struct
    {
        // First contact only: the bound socket, and the judgement on who may connect to it. A requester
        // past this point is reached through its own descriptor and never through this one.
        Listener admitting;

        // Every descriptor this loop watches, the listener and each accepted requester alike.
        posix::Epoll watching;

        // Nobody's requester: this is how another thread or a signal handler ends a pass that is asleep.
        posix::EventFd stopSignal;
    } polling_;

    // ── what an answer is made of ──────────────────────────────────────
    struct
    {
        // The one area, handed to every requester in turn. Borrowed, not given: the copy a requester
        // ends up with is the kernel's, made from this one when the Welcome goes out.
        posix::FileDesc areaDescriptor;

        // The setup message, answered once per connection. What it hands over is the area above.
        HelloExchange greeting;

        // The four a requester sends after that, and what gives a dead peer's domains back.
        DomainRequestExchange domainRequests;
    } answers_;

    // ── who is connected ───────────────────────────────────────────────
    // One entry per admitted requester, keyed by the descriptor the epoll carries back. Held rather
    // than inline: the entry's shape is the .cpp's, so no includer of this header depends on it.
    std::unique_ptr<Peers_t> peers_;

    // ── what the run is doing ──────────────────────────────────────────
    struct
    {
        // The loop's own, read at the top of each pass and cleared by stop().
        std::atomic<bool> running{true};

        // How long one wait may park. A pass reaches its top on this even with nobody connecting.
        timing::Millis idleInterval;

        // How many requesters this loop holds at once. Admission already turns away accounts the
        // policy does not name, so what this bounds is one permitted account spending the fd table.
        std::uint32_t maxConnections;
    } state_;

    // ── what a measurement asks afterwards ─────────────────────────────
    // Nothing here is acted on. Its own group, so a word that steers the loop is never one of these.
    struct
    {
        observe::EventCounts<observe::ControlEvent> events;
    } observe_;
};

}  // namespace cmed::daemon
