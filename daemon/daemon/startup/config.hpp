// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/startup/config.hpp -- the settings the daemon gets to choose. Not installed: a requester links
// the client library and never opens a region, so region uri, coherency and cohort cap are settings
// for a process it does not run. The client's file and this one are the same deployment file.

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/timing.hpp"
#include "shared/protocol/socket_path.hpp"

namespace cmed::daemon
{

// Which requesters the daemon lets onto its socket. Empty admits this daemon's uid alone.
struct AdmitPolicy_t
{
    std::vector<::uid_t> uids;
    std::vector<::gid_t> gids;
};

// Where the daemon looks when nobody passed --config.
inline constexpr const char* DefaultConfigPath = "/etc/cme/cmed.yaml";

// One group per reader, and each group's path is the path its key has in the file.
struct DaemonConfig_t
{
    // ── area ───────────────────────────────────────────────────────
    // Named once and used by everything below it: the socket path, the lock path, and the status line a
    // service manager shows. The area itself is anonymous, so this names the run rather than a file.
    struct
    {
        std::string name{"cmed"};
    } area;

    // ── socket ─────────────────────────────────────────────────────
    // Read by the control loop's listener, once, when it binds.
    struct
    {
        // Under systemd this is RuntimeDirectory, made fresh per start and owned by the daemon's uid,
        // which keeps another account from binding a socket at this path first.
        std::string dir{"/run/cmed"};

        // Applied after the bind, since the umask would narrow it. Who may connect is not the
        // boundary here: the daemon reads the peer's credentials and decides.
        mode_t mode{0660};
    } socket;

    // ── admission ──────────────────────────────────────────────────
    // Read by that same listener on every connect, which is not yet the way in: the area's mode still
    // draws the boundary until a requester receives its mapping over a connection.
    AdmitPolicy_t admit;

    // ── region ─────────────────────────────────────────────────────
    // Read once, by ServedArea at startup. Opened and never formatted: a format zeroes the region, and
    // the other nodes are using it.
    struct
    {
        std::string uri{"shm:/cme-region"};

        // Named rather than typed, so this header stays clear of libcme.
        std::string coherency{"cache_coherent"};

        // How long open() waits for a format another process started.
        timing::Millis formatTimeout{5000};
    } region;

    // ── cohort ─────────────────────────────────────────────────────
    // Read by DomainManager on a worker thread, once per turn it takes.
    struct
    {
        // How long one held CXL ownership may serve local grants before the turn goes back.
        timing::Millis hold{150};

        // How long a grant is worth acting on. Under the region's failure-detector window less a
        // margin, so a requester stops before the region hands the domain to another node.
        timing::Millis grantValidity{400};
    } cohort;

    // ── workers ────────────────────────────────────────────────────
    // Read by DomainWorkers when the pool starts. The dispatcher hands each pending domain to one of
    // these and never blocks itself, so a region acquire happens here.
    struct
    {
        // Set it for the host: zero is refused rather than read as a request to choose.
        std::uint32_t count{4};

        // How long a worker spins on its own word before sleeping. Buys the wake a syscall would cost
        // the dispatcher per hand-over, not the sleep itself.
        timing::Micros spin{10};
    } workers;

    // ── serve ──────────────────────────────────────────────────────
    // Both read by the dispatcher. Its spin is apart from the pool's above because one thread spinning
    // holds one core, while the pool spinning holds one per worker.
    struct
    {
        timing::Micros spin{200};

        // Bound on one doorbell wait. What it buys is a pass reaching the top with nothing ringing,
        // which is where work no requester announces gets seen: a peer that died rings nothing.
        timing::Millis idleInterval{200};
    } serve;

    // ── control ────────────────────────────────────────────────────
    // Read by the control loop as the bound on one epoll wait, so a loop nobody is connecting to still
    // reaches its top and can be told to leave.
    struct
    {
        timing::Millis idleInterval{200};

        // How many requesters this daemon holds at once. Admission already turns away accounts the
        // policy does not name, so what this bounds is one permitted account spending the fd table.
        std::uint32_t maxConnections{256};
    } control;

    // ── maintenance ────────────────────────────────────────────────
    // Read by the maintainer thread as the pace of one pass, which is the registry refresh and the
    // expired-turn sweep together.
    struct
    {
        timing::Millis interval{20};
    } maintenance;

    // ── registry ───────────────────────────────────────────────────
    // Both read by DomainManager, on different threads: the refresh on the maintainer's, the wait on the
    // control thread answering a delete.
    struct
    {
        // How long a domain another node created can go unseen by this node's requesters.
        timing::Millis refreshInterval{1000};

        // How long a delete waits for a domain to fall idle before answering EBUSY. Short, since a
        // requester inside its critical section is not late and this wait holds the control thread.
        timing::Millis deleteWaitTimeout{100};
    } registry;

    // ── derived ────────────────────────────────────────────────────
    [[nodiscard]] std::string socketPath() const
    {
        return protocol::buildSocketPath(socket.dir, area.name);
    }

    // What one daemon per area is enforced on, beside the socket since both name the same run.
    [[nodiscard]] std::string lockPath() const
    {
        return socket.dir + "/" + area.name + ".lock";
    }

    // What a service manager shows beside the unit: the two names a deployment gave this run, which is
    // what tells two daemons on one host apart in `systemctl status`.
    [[nodiscard]] std::string statusLine() const
    {
        return "area " + area.name + ", region " + region.uri;
    }
};

[[nodiscard]] DaemonConfig_t loadDaemonConfig(const std::string& path);

}  // namespace cmed::daemon
