// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/datagram_socket.hpp -- one SOCK_DGRAM socket, sender or receiver.
//
// Apart from SeqpacketSocket because the two answer different questions. A datagram carries its own
// boundary and needs no connection, so nothing here can say whether a peer is still there.
//
// It exists for one protocol: a service manager hands over an address, takes one message, and never
// replies. The receiving side is how a probe stands in for that manager.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/timing.hpp"
#include "shared/posix/socket.hpp"

namespace posix
{

class DatagramSocket
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    DatagramSocket(const DatagramSocket&) = delete;
    DatagramSocket(DatagramSocket&&) noexcept = default;
    ~DatagramSocket() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────
    // No move assignment: a factory's result is what a caller initialises from, and nothing here
    // assigns over a socket that already bound a name.
    DatagramSocket& operator=(const DatagramSocket&) = delete;
    DatagramSocket& operator=(DatagramSocket&&) = delete;

    // ── factories ──────────────────────────────────────────────────
    // Bound to no name of its own and connected to @path, so every send after this states no address.
    // Throws what the connect throws: an address nothing answers at is not one to keep a socket for.
    [[nodiscard]] static DatagramSocket sender(const std::string& path);

    // Bound to @path, so a datagram addressed there arrives here. Owns the name. In a deployment the
    // receiving end is systemd, so this stands in for it wherever the notification itself is observed.
    [[nodiscard]] static DatagramSocket receiver(std::string path);

    // ── public methods ─────────────────────────────────────────────
    // Best effort and noexcept: a caller announcing itself has nothing better to do about a failure
    // than carry on. MSG_NOSIGNAL, so a closed peer does not end it.
    [[nodiscard]] bool send(std::string_view bytes) const noexcept;

    // One datagram of at most @most bytes, or nothing within @timeout. Throws std::system_error when
    // the socket itself fails. Not const: what it returns leaves the queue, so a second call differs.
    [[nodiscard]] std::optional<std::string> receive(timing::Millis timeout, std::uint32_t most);

    // ── accessors ──────────────────────────────────────────────────
    // The path this socket bound, empty for a sender. What a receiver hands to whoever will send.
    [[nodiscard]] const std::string& name() const noexcept
    {
        return socket_.name();
    }

private:
    explicit DatagramSocket(Socket socket) noexcept;

private:
    Socket socket_;
};

}  // namespace posix
