// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/seqpacket_socket.hpp -- one SOCK_SEQPACKET socket, and the calls it takes to get one.
//
// Binding is several calls with asymmetric failure: past the bind a name exists that a throw has to
// take with it, which is what a missed unlink at a call site would leave behind.
//
// The type is what buys accept, descriptor passing and peer credentials: no caller frames a length,
// and a connection is what says the peer is gone.

#pragma once

#include <sys/socket.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/timing.hpp"
#include "shared/posix/socket.hpp"
#include "shared/posix/unique_fd.hpp"

namespace posix
{

// The credentials the kernel stamped on the peer at connect time, or nothing when the socket cannot
// answer for them. Stamped rather than asked for, so a recycled pid cannot be the one answering.
[[nodiscard]] std::optional<::ucred> readPeer(FileDesc socket) noexcept;

class SeqpacketSocket
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    SeqpacketSocket() noexcept = default;

    SeqpacketSocket(const SeqpacketSocket&) = delete;
    SeqpacketSocket(SeqpacketSocket&&) noexcept = default;
    ~SeqpacketSocket() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────
    SeqpacketSocket& operator=(const SeqpacketSocket&) = delete;
    SeqpacketSocket& operator=(SeqpacketSocket&&) noexcept = default;

    // ── factories ──────────────────────────────────────────────────
    // Binds @path, sets @mode on it by path (a socket is not a chmod target), and listens. A name
    // already there is removed first: the directory is the caller's own, so it is its own leftover.
    [[nodiscard]] static SeqpacketSocket listen(std::string path, ::mode_t mode, std::int32_t backlog);

    // The other end. It owns no name: the path belongs to whoever bound it.
    [[nodiscard]] static SeqpacketSocket connect(const std::string& path);

    // A connection someone else accepted. It owns no name, because the path belongs to whoever bound
    // it and outlives this one end of it.
    [[nodiscard]] static SeqpacketSocket adopt(UniqueFd held) noexcept;

    // ── nested types ───────────────────────────────────────────────
    // What one message carried. The descriptors are owned on arrival: a received one that nobody
    // adopted is a descriptor leaked into a process that never asked for it.
    struct Received_t
    {
        std::string message;
        std::vector<UniqueFd> descriptors;
    };

    // ── public methods ─────────────────────────────────────────────
    // One waiting connection, or nothing when none is waiting. A peer that went away between its
    // connect and this call reads as nothing too: both leave the caller with other work to do.
    [[nodiscard]] UniqueFd accept();

    // Sends @bytes with @descriptors attached. The kernel duplicates them into the receiver, so the
    // caller keeps its own and closes them on its own schedule.
    [[nodiscard]] bool send(std::string_view bytes, const std::vector<FileDesc>& descriptors) const;

    // One message, or nothing when the peer has gone; a message exceeding @mostBytes or
    // @mostDescriptors is refused, since a truncated control buffer loses descriptors the sender sent.
    //
    // @waitFor is set every call, so no deadline outlives the call that asked for it. Absent means
    // none, which is what a caller epoll already told there is a message to read wants.
    [[nodiscard]] std::optional<Received_t>
    receive(std::uint32_t mostBytes, std::uint32_t mostDescriptors,
            std::optional<timing::Millis> waitFor = {});

    // Whether the peer has gone, asked without taking anything off the socket. A message waiting to be
    // read is not a hangup, so only the three bits that say the other end is finished are looked at.
    [[nodiscard]] bool isPeerGone() const noexcept;

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] FileDesc descriptor() const noexcept
    {
        return socket_.descriptor();
    }

    explicit operator bool() const noexcept
    {
        return static_cast<bool>(socket_);
    }

private:
    // What sizes the control buffer on the stack. A ceiling and not a tunable: a caller asking for more
    // is refused rather than served from the heap, and what it asks for is still its own contract.
    static constexpr std::uint32_t MostDescriptorsEver = 8;

    // That many descriptors, their cmsghdr, and the padding behind it. CMSG_SPACE and not CMSG_LEN,
    // which stops at the data and would undercount the buffer.
    static constexpr std::size_t ControlBytes = CMSG_SPACE(MostDescriptorsEver * sizeof(FileDesc));

    explicit SeqpacketSocket(Socket socket) noexcept;

private:
    Socket socket_;
};

}  // namespace posix
