// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/startup/service_notifier.cpp -- see service_notifier.hpp.

#include "daemon/startup/service_notifier.hpp"

#include <cstdlib>
#include <string>

#include "shared/posix/datagram_socket.hpp"

namespace cmed::daemon
{

// ── what the environment said ──────────────────────────────────────

namespace
{

// A service manager passes an absolute path or an abstract name. Any other spelling is not an address
// this protocol defines, and guessing at one is worse than staying quiet.
[[nodiscard]] std::string readAddress()
{
    const char* const named = ::getenv("NOTIFY_SOCKET");
    if (named == nullptr || (*named != '/' && *named != '@'))
    {
        return {};
    }
    return named;
}

}  // namespace

// ── ctor / dtor ────────────────────────────────────────────────────

ServiceNotifier::ServiceNotifier()
{
    const auto address = readAddress();
    if (address.empty())
    {
        return;
    }

    try
    {
        sender_.emplace(posix::DatagramSocket::sender(address));
    }
    catch (...)
    {
        // @expected: an address nothing answers at is a manager this cannot reach, which is not a reason
        // to refuse to serve the requesters it can.
    }
}

// ── public methods ─────────────────────────────────────────────────

void ServiceNotifier::notifyReady(const std::string& status) const noexcept
{
    send("READY=1\nSTATUS=" + status + "\n");
}

void ServiceNotifier::notifyAlive() const noexcept
{
    send("WATCHDOG=1\n");
}

// ── one datagram ───────────────────────────────────────────────────

void ServiceNotifier::send(const std::string& message) const noexcept
{
    if (!sender_)
    {
        return;
    }

    // Best effort: a manager that stopped listening is not a reason to stop serving the requesters that
    // are. The send itself is noexcept, so nothing here can throw.
    static_cast<void>(sender_->send(message));
}

}  // namespace cmed::daemon
