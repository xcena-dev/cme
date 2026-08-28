// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/socket.cpp -- see socket.hpp. Every use of sockaddr_un is here, which is what keeps
// <sys/un.h> out of the two socket headers and out of everything that includes them.

#include "shared/posix/socket.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "shared/posix/unique_fd.hpp"

namespace posix
{

namespace
{

// One AF_UNIX address, in the two pieces every socket call wants it in. A leading '/' is a
// filesystem path and '@' is the abstract namespace, whose first byte is NUL.
class Address
{
public:
    // Throws std::invalid_argument for a name sun_path cannot hold, since a truncated one names
    // somewhere else rather than failing.
    explicit Address(const std::string& path)
    {
        if (path.empty() || path.size() >= sizeof(value_.sun_path))
        {
            throw std::invalid_argument{"unix socket address does not fit sun_path: " + path};
        }

        value_.sun_family = AF_UNIX;
        std::memcpy(value_.sun_path, path.data(), path.size());

        const bool abstract = path.front() == '@';
        if (abstract)
        {
            value_.sun_path[0] = '\0';
        }

        // Not sizeof: an abstract name ends where the length says, and a pathname includes its
        // terminator, so the two forms differ by one byte.
        length_ = static_cast<::socklen_t>(offsetof(::sockaddr_un, sun_path) + path.size() +
                                           (abstract ? 0U : 1U));
    }

    // The same address as the family-neutral type the socket calls declare. C has no conversion for
    // it, so the cast is the convention and this is the one place it is written.
    [[nodiscard]] const ::sockaddr* asSockaddr() const noexcept
    {
        return reinterpret_cast<const ::sockaddr*>(&value_);
    }

    [[nodiscard]] ::socklen_t length() const noexcept
    {
        return length_;
    }

private:
    ::sockaddr_un value_{};
    ::socklen_t length_{};
};

}  // namespace

Socket::Socket(Socket&& other) noexcept
    : held_{std::move(other.held_)},
      named_{std::exchange(other.named_, {})}
{
}

// The unlink result goes unread: the socket is going either way, and a name already gone is the
// outcome this wanted.
Socket::~Socket() noexcept
{
    if (hasPathname())
    {
        ::unlink(named_.c_str());
    }
}

// Written out because the default one drops the old name instead of removing it: string assignment
// has no reason to unlink, and the file would outlive the socket it stood for.
Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other)
    {
        if (hasPathname())
        {
            ::unlink(named_.c_str());
        }
        held_ = std::move(other.held_);
        named_ = std::exchange(other.named_, {});
    }
    return *this;
}

std::system_error Socket::error(const std::string& what)
{
    return std::system_error{errno, std::system_category(), what};
}

Socket Socket::open(std::int32_t type)
{
    UniqueFd held{::socket(AF_UNIX, type, 0)};
    if (!held)
    {
        throw error("socket");
    }
    return Socket{std::move(held)};
}

Socket Socket::adopt(UniqueFd held) noexcept
{
    return Socket{std::move(held)};
}

void Socket::bind(std::string path)
{
    const Address address{path};

    // unlink before bind, because the directory is the caller's own and a name sitting there is its
    // own leftover rather than another process's claim. An abstract name has no file to remove.
    ::unlink(path.c_str());
    if (::bind(held_.get(), address.asSockaddr(), address.length()) != 0)
    {
        throw error("bind(" + path + ")");
    }

    // After the bind, so a throw above leaves nothing for the destructor to remove.
    named_ = std::move(path);
}

void Socket::connect(const std::string& path)
{
    const Address address{path};
    if (::connect(held_.get(), address.asSockaddr(), address.length()) != 0)
    {
        throw error("connect(" + path + ")");
    }
}

bool Socket::send(std::string_view bytes) const noexcept
{
    const ::ssize_t sent = ::send(held_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
    return sent == static_cast<::ssize_t>(bytes.size());
}

bool Socket::hasPathname() const noexcept
{
    return !named_.empty() && named_.front() != '@';
}

Socket::Socket(UniqueFd held) noexcept
    : held_{std::move(held)}
{
}

}  // namespace posix
