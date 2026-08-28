// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// listener_probe.cpp -- who reaches the daemon's socket, and who is turned away at it.
//
// The admission boundary moves here from the area's file mode, so this tests a judgement rather
// than a permission: the kernel stamps the connecting process's credentials at connect time.
//
// A relative path on purpose: sun_path is a fixed field, and the build tree's absolute path plus
// a name is close enough to that ceiling to fail for an unrelated reason.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

#include "daemon/control/listener.hpp"
#include "daemon/startup/config.hpp"
#include "shared/posix/seqpacket_socket.hpp"

namespace
{

constexpr const char* SocketPath = "listener_probe.sock";
constexpr mode_t SocketMode = 0660;

// An empty policy is the narrowest one, so only the process that started the daemon gets in.
// Both credentials are checked, so a listener admitting everyone would not pass this case.
bool admitsTheProcessThatConnected()
{
    cmed::daemon::Listener listening{SocketPath, SocketMode, cmed::daemon::AdmitPolicy_t{}};
    const auto reaching = posix::SeqpacketSocket::connect(SocketPath);

    const cmed::daemon::Listener::Admitted_t arrival = listening.take();

    return arrival.outcome == cmed::daemon::Listener::Admission::Accepted &&
           arrival.peer.uid == ::geteuid() && arrival.peer.pid == ::getpid();
}

// A policy naming one uid this process does not have. The refusal still reports who was refused,
// which is what makes an audit line worth writing.
bool refusesAUidNotInThePolicy()
{
    cmed::daemon::AdmitPolicy_t policy;
    policy.uids.push_back(::geteuid() + 1);

    cmed::daemon::Listener listening{SocketPath, SocketMode, policy};
    const auto reaching = posix::SeqpacketSocket::connect(SocketPath);

    const cmed::daemon::Listener::Admitted_t arrival = listening.take();

    return arrival.outcome == cmed::daemon::Listener::Admission::Refused && !arrival.socket &&
           arrival.peer.uid == ::geteuid();
}

// The gid path, on a group this process already has. Its own case because a policy matching on
// neither uid nor gid would still pass the one above.
bool admitsAGidInThePolicy()
{
    cmed::daemon::AdmitPolicy_t policy;
    policy.uids.push_back(::geteuid() + 1);
    policy.gids.push_back(::getegid());

    cmed::daemon::Listener listening{SocketPath, SocketMode, policy};
    const auto reaching = posix::SeqpacketSocket::connect(SocketPath);

    return listening.take().outcome == cmed::daemon::Listener::Admission::Accepted;
}

// Nobody waiting is not a failure. A loop with other work has to be told to go do it.
bool answersAgainWithNobodyWaiting()
{
    cmed::daemon::Listener listening{SocketPath, SocketMode, cmed::daemon::AdmitPolicy_t{}};

    return listening.take().outcome == cmed::daemon::Listener::Admission::Again;
}

// bind is masked by the umask, so a deployment asking for group access under UMask=0077 would
// silently not get it. The resulting mode is asserted rather than the call that set it.
bool carriesTheModeItWasAsked()
{
    const cmed::daemon::Listener listening{SocketPath, SocketMode, cmed::daemon::AdmitPolicy_t{}};

    struct ::stat found = {};
    if (::stat(SocketPath, &found) != 0)
    {
        return false;
    }
    return (found.st_mode & 0777) == SocketMode;
}

// A truncated address binds somewhere other than where the caller said, which is worse than
// refusing to bind at all.
bool refusesAPathThatDoesNotFit()
{
    const std::string tooLong(120, 'x');
    try
    {
        const cmed::daemon::Listener listening{tooLong, SocketMode, cmed::daemon::AdmitPolicy_t{}};
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }
    return false;
}

// A name outliving its listener is a path a requester can connect to and never be answered on.
bool takesItsNameWithIt()
{
    {
        const cmed::daemon::Listener listening{SocketPath, SocketMode, cmed::daemon::AdmitPolicy_t{}};
    }

    struct ::stat found = {};
    return ::stat(SocketPath, &found) != 0;
}

}  // namespace

int main()
{
    ::unlink(SocketPath);

    bool passed = false;
    try
    {
        passed = admitsTheProcessThatConnected() && refusesAUidNotInThePolicy() &&
                 admitsAGidInThePolicy() && answersAgainWithNobodyWaiting() &&
                 carriesTheModeItWasAsked() && refusesAPathThatDoesNotFit() && takesItsNameWithIt();
    }
    catch (const std::exception& failure)
    {
        std::printf("listener probe threw: %s\n", failure.what());
        return 1;
    }

    ::unlink(SocketPath);
    std::printf("listener probe %s\n", passed ? "ok" : "failed");
    return passed ? 0 : 1;
}
