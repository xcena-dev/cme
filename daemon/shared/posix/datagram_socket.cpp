// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/datagram_socket.cpp -- see datagram_socket.hpp.

#include "shared/posix/datagram_socket.hpp"

#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/timing.hpp"
#include "shared/posix/socket.hpp"

namespace posix
{

DatagramSocket::DatagramSocket(Socket socket) noexcept
    : socket_{std::move(socket)}
{
}

DatagramSocket DatagramSocket::sender(const std::string& path)
{
    Socket reaching = Socket::open(SOCK_DGRAM | SOCK_CLOEXEC);
    reaching.connect(path);

    return DatagramSocket{std::move(reaching)};
}

DatagramSocket DatagramSocket::receiver(std::string path)
{
    Socket bound = Socket::open(SOCK_DGRAM | SOCK_CLOEXEC);
    bound.bind(std::move(path));

    return DatagramSocket{std::move(bound)};
}

bool DatagramSocket::send(std::string_view bytes) const noexcept
{
    return socket_.send(bytes);
}

std::optional<std::string> DatagramSocket::receive(timing::Millis timeout, std::uint32_t most)
{
    const ::timeval bound = timing::toTimeval(timeout);
    if (::setsockopt(socket_.descriptor(), SOL_SOCKET, SO_RCVTIMEO, &bound, sizeof(bound)) != 0)
    {
        throw Socket::error("setsockopt(SO_RCVTIMEO)");
    }

    std::string taken(most, '\0');
    const ::ssize_t got = ::recv(socket_.descriptor(), taken.data(), taken.size(), 0);
    if (got < 0)
    {
        // Running out of time is the expected way to get nothing, and a signal leaves the caller free
        // to ask again. Anything else is the socket itself failing and is not a quiet empty answer.
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            return std::nullopt;
        }
        throw Socket::error("recv");
    }

    // Zero is a datagram of no bytes, not an absent one: a connectionless socket has no peer whose
    // going away it could stand for.
    taken.resize(static_cast<std::string::size_type>(got));
    return taken;
}

}  // namespace posix
