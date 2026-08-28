// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/socket.hpp -- one AF_UNIX socket and the name it put itself at, if it took one.
//
// The name goes when the socket does: a name outliving the socket behind it is a path a client can
// reach and never be answered on. Addressing stays in the source, so <sys/un.h> reaches no consumer.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#include "shared/posix/unique_fd.hpp"

namespace posix
{

class Socket
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    Socket() noexcept = default;

    Socket(const Socket&) = delete;

    // The name goes with the descriptor, so the source is left holding neither.
    Socket(Socket&&) noexcept;

    ~Socket() noexcept;

    // ── operator= ──────────────────────────────────────────────────
    Socket& operator=(const Socket&) = delete;
    Socket& operator=(Socket&&) noexcept;

    // ── factories ──────────────────────────────────────────────────
    // One socket of @type, carrying the SOCK_ flags the caller wants, at no name yet. Throws
    // std::system_error carrying errno, so no caller is left holding a descriptor that is -1.
    [[nodiscard]] static Socket open(std::int32_t type);

    // Wraps a descriptor someone else obtained. It holds no name, since it bound none.
    [[nodiscard]] static Socket adopt(UniqueFd held) noexcept;

    // ── public methods ─────────────────────────────────────────────
    // Puts this socket at @path and takes the name with it, so binding and owning the cleanup are one
    // step. A name already there is removed first: the directory is the caller's own leftover.
    void bind(std::string path);

    // Reaches @path. Throws std::invalid_argument for a name sun_path cannot hold and
    // std::system_error carrying errno when the connect itself fails.
    void connect(const std::string& path);

    // One datagram to whatever connect() reached, so the address is stated once and not per message.
    // Best effort and noexcept, since a caller announcing itself has nothing better to do about a
    // failure than carry on.
    [[nodiscard]] bool send(std::string_view bytes) const noexcept;

    // ── accessors ──────────────────────────────────────────────────
    // errno as an exception, for the calls a caller makes on the descriptor itself.
    [[nodiscard]] static std::system_error error(const std::string& what);

    [[nodiscard]] FileDesc descriptor() const noexcept
    {
        return held_.get();
    }

    // Empty for a socket that bound nothing. What a bound one hands to whoever will reach it.
    [[nodiscard]] const std::string& name() const noexcept
    {
        return named_;
    }

    explicit operator bool() const noexcept
    {
        return static_cast<bool>(held_);
    }

private:
    // True for the one address form unix(7) puts in the filesystem. An abstract name lives in the
    // kernel, so unlinking it would reach a relative path of the same spelling instead.
    [[nodiscard]] bool hasPathname() const noexcept;

    explicit Socket(UniqueFd held) noexcept;

private:
    UniqueFd held_;
    std::string named_;
};

}  // namespace posix
