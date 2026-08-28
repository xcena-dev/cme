// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_socket.hpp -- both ends of one connection, in one process.
//
// The real pair is two processes, and nothing a case here asks depends on which: a descriptor and a
// message cross the same way either way. What one process buys is that a case can drop either end
// and read what the other one then sees.
//
// The listening socket is kept because it owns the name: dropping it would unlink the path while the
// two ends are still connected through it.

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <utility>

#include "shared/posix/seqpacket_socket.hpp"

namespace cmed::harness
{

// A relative path stays relative on purpose. sun_path is a fixed field, and a build tree's absolute
// path plus a name is close enough to that ceiling to fail for a reason unrelated to the case.
struct ConnectedPair_t
{
    posix::SeqpacketSocket listening;
    posix::SeqpacketSocket serving;
    posix::SeqpacketSocket asking;
};

// @path is bound and removed with the listening socket. @mode is applied after the bind, since the
// umask would otherwise narrow it.
[[nodiscard]] inline ConnectedPair_t connectSocketPair(const std::string& path, ::mode_t mode)
{
    constexpr std::int32_t Backlog = 4;

    auto listening = posix::SeqpacketSocket::listen(path, mode, Backlog);
    auto asking = posix::SeqpacketSocket::connect(path);
    auto serving = posix::SeqpacketSocket::adopt(listening.accept());

    return ConnectedPair_t{std::move(listening), std::move(serving), std::move(asking)};
}

}  // namespace cmed::harness
