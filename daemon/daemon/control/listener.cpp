// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/control/listener.cpp -- see listener.hpp.

#include "daemon/control/listener.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "daemon/startup/config.hpp"
#include "shared/posix/seqpacket_socket.hpp"
#include "shared/posix/unique_fd.hpp"

namespace cmed::daemon
{

namespace
{

// Deep enough that requesters starting together are queued rather than refused, and no deeper: a
// queue nobody accepts from is a queue of connections timing out.
constexpr std::int32_t Backlog = 16;

}  // namespace

bool admits(const AdmitPolicy_t& policy, const ::ucred& peer) noexcept
{
    if (policy.uids.empty() && policy.gids.empty())
    {
        return peer.uid == ::geteuid();
    }

    for (const ::uid_t uid : policy.uids)
    {
        if (peer.uid == uid)
        {
            return true;
        }
    }
    for (const ::gid_t gid : policy.gids)
    {
        if (peer.gid == gid)
        {
            return true;
        }
    }

    return false;
}

Listener::Listener(const std::string& path, mode_t mode, AdmitPolicy_t admit)
    : socket_{posix::SeqpacketSocket::listen(path, mode, Backlog)},
      admit_{std::move(admit)}
{
}

Listener::Admitted_t Listener::take()
{
    Admitted_t admitted;

    posix::UniqueFd taken = socket_.accept();
    if (!taken)
    {
        return admitted;
    }

    const std::optional<::ucred> peer = posix::readPeer(taken.get());
    if (!peer)
    {
        // No credentials, so there is no judgement to make and no grounds to admit.
        admitted.outcome = Admission::Refused;
        return admitted;
    }

    admitted.peer = *peer;
    if (!admits(admit_, admitted.peer))
    {
        admitted.outcome = Admission::Refused;
        return admitted;
    }

    admitted.socket = std::move(taken);
    admitted.outcome = Admission::Accepted;
    return admitted;
}

}  // namespace cmed::daemon
