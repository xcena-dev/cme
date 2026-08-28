// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/control/listener.hpp -- who reaches the daemon, and who is turned away. Admission moves from a
// file's mode to the daemon's judgement, using the peer credentials the kernel stamps at connect
// time. The socket itself is posix::SeqpacketSocket; what is left here is that judgement alone.

#pragma once

#include <sys/socket.h>
#include <sys/types.h>

#include <string>

#include "daemon/startup/config.hpp"
#include "shared/posix/seqpacket_socket.hpp"
#include "shared/posix/unique_fd.hpp"

namespace cmed::daemon
{

// An empty policy admits this daemon's uid alone. ucred carries the primary gid only, so a requester
// whose membership in an admitted group is supplementary needs SO_PEERGROUPS to be seen at all.
[[nodiscard]] bool admits(const AdmitPolicy_t& policy, const ::ucred& peer) noexcept;

class Listener
{
public:
    // ── nested types ───────────────────────────────────────────────
    enum class Admission
    {
        Accepted,  // socket carries the connection
        Refused,   // socket holds nothing; peer still says who was turned away
        Again,     // nobody is waiting
    };

    // Move-only, because it owns a descriptor. A refused arrival closes it by going out of scope,
    // which is why no case here has a close to forget.
    struct Admitted_t
    {
        Admission outcome{Admission::Again};
        posix::UniqueFd socket;
        ::ucred peer{};
    };

    // ── ctor / dtor ────────────────────────────────────────────────
    // Throws std::system_error carrying errno when a call fails, and std::invalid_argument for a path
    // that does not fit an address. Nothing is left behind on either.
    Listener(const std::string& path, mode_t mode, AdmitPolicy_t admit);

    Listener(const Listener&) = delete;
    Listener(Listener&&) = delete;

    // Nothing to do: the socket closes its descriptor and removes its own name.
    ~Listener() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────
    Listener& operator=(const Listener&) = delete;
    Listener& operator=(Listener&&) = delete;

    // ── public methods ─────────────────────────────────────────────
    // Takes one connection if one is waiting. Non-blocking: the caller is a loop with other work,
    // and Again is how it is told to go do that work.
    [[nodiscard]] Admitted_t take();

    // ── accessors ──────────────────────────────────────────────────
    // For the epoll the control loop waits in.
    [[nodiscard]] posix::FileDesc descriptor() const noexcept
    {
        return socket_.descriptor();
    }

private:
    posix::SeqpacketSocket socket_;
    AdmitPolicy_t admit_;
};

}  // namespace cmed::daemon
