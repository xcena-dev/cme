// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/startup/service_notifier.hpp -- what this daemon tells the service manager that started it:
// that it is serving, what to show in a status line, and that it is still alive. Datagrams rather than
// libsystemd, and silent when NOTIFY_SOCKET is unset, which is every run outside a manager.

#pragma once

#include <optional>
#include <string>

#include "shared/posix/datagram_socket.hpp"

namespace cmed::daemon
{

class ServiceNotifier
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // Reads NOTIFY_SOCKET once and connects once. Neither can change while this daemon runs, and a
    // socket per message would spend three syscalls where one datagram needs one.
    ServiceNotifier();

    // ── public methods ─────────────────────────────────────────────
    // The READY=1 the manager waits for, with @status where `systemctl status` shows it.
    void notifyReady(const std::string& status) const noexcept;

    // The WATCHDOG=1 that resets the manager's timer. A manager that armed none has no timestamp to
    // reset and does nothing with it, so no caller here has to ask whether one was armed.
    void notifyAlive() const noexcept;

private:
    // ── one datagram ───────────────────────────────────────────────
    // Silent without a socket, so no caller above has to ask whether a manager is there at all.
    void send(const std::string& message) const noexcept;

private:
    // Connected to the manager's address and held open for the whole run. Empty when no manager named
    // an address, and empty when the one it named could not be reached.
    std::optional<posix::DatagramSocket> sender_;
};

}  // namespace cmed::daemon
