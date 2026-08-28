// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/seqpacket_socket.cpp -- see seqpacket_socket.hpp.

#include "shared/posix/seqpacket_socket.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/timing.hpp"
#include "shared/posix/socket.hpp"
#include "shared/posix/unique_fd.hpp"

namespace posix
{

namespace
{

// The three kernel structs one message is built from: the message as a whole, the payload it points at,
// and one control header inside its control buffer. Only this file speaks them, so the names stay here.
using MessageHeader = ::msghdr;
using PayloadSpan = ::iovec;
using ControlHeader = ::cmsghdr;

// The same length for the header field, which counts the data and not the padding behind it.
[[nodiscard]] std::size_t readControlLength(std::size_t descriptors) noexcept
{
    return CMSG_SPACE(descriptors * sizeof(FileDesc));
}

// ── the cmsg macros, as functions ──────────────────────────────────
// Each is a macro that reads or writes through @message, so a caller cannot tell by looking whether it
// is a plain computation. Named here once, so the walk below reads as a walk.

[[nodiscard]] ControlHeader* readFirstHeader(MessageHeader& message) noexcept
{
    return CMSG_FIRSTHDR(&message);
}

[[nodiscard]] ControlHeader* readNextHeader(MessageHeader& message, ControlHeader* header) noexcept
{
    return CMSG_NXTHDR(&message, header);
}

// Where the payload starts, past the header and its alignment.
[[nodiscard]] std::uint8_t* readHeaderData(ControlHeader* header) noexcept
{
    return CMSG_DATA(header);
}

// How many descriptors this header carries. cmsg_len counts the header too, so CMSG_LEN(0) is what
// that header costs before any payload.
[[nodiscard]] std::uint32_t readCarriedCount(const ControlHeader* header) noexcept
{
    return static_cast<std::uint32_t>((header->cmsg_len - CMSG_LEN(0)) / sizeof(FileDesc));
}

[[nodiscard]] bool hasDescriptors(const ControlHeader* header) noexcept
{
    return header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS;
}

// Every descriptor in every SCM_RIGHTS header, in arrival order. One message may carry more than one
// header, and a descriptor missed here is one nothing will ever close.
void adoptDescriptors(MessageHeader& message, std::vector<UniqueFd>& into)
{
    for (auto* header = readFirstHeader(message); header != nullptr; header = readNextHeader(message, header))
    {
        if (!hasDescriptors(header))
        {
            continue;
        }

        const auto carried = readCarriedCount(header);
        for (std::uint32_t index = 0; index < carried; ++index)
        {
            FileDesc taken = -1;
            std::memcpy(&taken, readHeaderData(header) + (index * sizeof(taken)), sizeof(taken));
            into.emplace_back(taken);
        }
    }
}

}  // namespace

std::optional<::ucred> readPeer(FileDesc socket) noexcept
{
    ::ucred peer = {};
    ::socklen_t width = sizeof(peer);
    if (::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &peer, &width) != 0)
    {
        return std::nullopt;
    }
    return peer;
}

SeqpacketSocket::SeqpacketSocket(Socket socket) noexcept
    : socket_{std::move(socket)}
{
}

SeqpacketSocket SeqpacketSocket::listen(std::string path, ::mode_t mode, std::int32_t backlog)
{
    Socket binding = Socket::open(SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK);

    // The name is the socket's from here on, so the two calls below can throw and still leave it to
    // the destructor rather than to this function.
    binding.bind(std::move(path));

    if (::chmod(binding.name().c_str(), mode) != 0)
    {
        throw Socket::error("chmod(" + binding.name() + ")");
    }
    if (::listen(binding.descriptor(), backlog) != 0)
    {
        throw Socket::error("listen(" + binding.name() + ")");
    }
    return SeqpacketSocket{std::move(binding)};
}

SeqpacketSocket SeqpacketSocket::connect(const std::string& path)
{
    Socket reaching = Socket::open(SOCK_SEQPACKET | SOCK_CLOEXEC);
    reaching.connect(path);

    return SeqpacketSocket{std::move(reaching)};
}

SeqpacketSocket SeqpacketSocket::adopt(UniqueFd held) noexcept
{
    return SeqpacketSocket{Socket::adopt(std::move(held))};
}

UniqueFd SeqpacketSocket::accept()
{
    UniqueFd taken{::accept4(socket_.descriptor(), nullptr, nullptr, SOCK_CLOEXEC)};
    if (taken)
    {
        return taken;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED)
    {
        return {};
    }
    throw Socket::error("accept");
}

bool SeqpacketSocket::send(std::string_view bytes, const std::vector<FileDesc>& descriptors) const
{
    if (descriptors.size() > MostDescriptorsEver)
    {
        throw std::invalid_argument{"more descriptors than one message may carry"};
    }

    std::array<char, ControlBytes> control{};

    PayloadSpan span = {};
    span.iov_base = const_cast<char*>(bytes.data());
    span.iov_len = bytes.size();

    MessageHeader message = {};
    message.msg_iov = &span;
    message.msg_iovlen = 1;
    if (!descriptors.empty())
    {
        // The space one header needs, not the whole buffer: the kernel walks msg_controllen looking for
        // another header, and the zeroes past this one are not a header it can read.
        message.msg_control = control.data();
        message.msg_controllen = readControlLength(descriptors.size());

        ControlHeader* const header = readFirstHeader(message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(descriptors.size() * sizeof(FileDesc));
        std::memcpy(readHeaderData(header), descriptors.data(), descriptors.size() * sizeof(FileDesc));
    }

    return ::sendmsg(socket_.descriptor(), &message, MSG_NOSIGNAL) == static_cast<::ssize_t>(bytes.size());
}

bool SeqpacketSocket::isPeerGone() const noexcept
{
    // POLLRDHUP is only reported when it is asked for. The other two arrive whether or not they are.
    // poll.h is the umbrella header; glibc declares these under bits/, which include-cleaner reads instead.
    // NOLINTBEGIN(misc-include-cleaner)
    ::pollfd watching = {};
    watching.fd = socket_.descriptor();
    watching.events = POLLRDHUP;

    if (::poll(&watching, 1, 0) <= 0)
    {
        // Nothing to report, or a call this cannot act on. Neither says the peer went.
        return false;
    }
    return (watching.revents & (POLLRDHUP | POLLHUP | POLLERR)) != 0;
    // NOLINTEND(misc-include-cleaner)
}

std::optional<SeqpacketSocket::Received_t>
SeqpacketSocket::receive(std::uint32_t mostBytes, std::uint32_t mostDescriptors,
                         std::optional<timing::Millis> waitFor)
{
    // Zero is how SO_RCVTIMEO spells "no deadline", so the absent case is a value like any other and
    // nothing carries over from an earlier call.
    const ::timeval bound = timing::toTimeval(waitFor.value_or(timing::Millis::zero()));
    if (::setsockopt(socket_.descriptor(), SOL_SOCKET, SO_RCVTIMEO, &bound, sizeof(bound)) != 0)
    {
        throw Socket::error("setsockopt(SO_RCVTIMEO)");
    }

    if (mostDescriptors > MostDescriptorsEver)
    {
        throw std::invalid_argument{"more descriptors than one message may carry"};
    }

    std::string taken(mostBytes, '\0');
    std::array<char, ControlBytes> control{};

    PayloadSpan span = {};
    span.iov_base = taken.data();
    span.iov_len = taken.size();

    MessageHeader message = {};
    message.msg_iov = &span;
    message.msg_iovlen = 1;
    // The buffer is the ceiling but the length is what the caller asked for, because that length is the
    // contract: a message carrying more than it said arrives truncated and is refused below.
    message.msg_control = control.data();
    message.msg_controllen = readControlLength(mostDescriptors);

    const ::ssize_t got = ::recvmsg(socket_.descriptor(), &message, MSG_CMSG_CLOEXEC);
    if (got <= 0)
    {
        return std::nullopt;
    }

    Received_t received;
    adoptDescriptors(message, received.descriptors);

    // After the adopt, so a refusal still closes what arrived rather than leaking it.
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0)
    {
        throw std::system_error{EMSGSIZE, std::system_category(), "recvmsg truncated"};
    }

    taken.resize(static_cast<std::string::size_type>(got));
    received.message = std::move(taken);
    return received;
}

}  // namespace posix
