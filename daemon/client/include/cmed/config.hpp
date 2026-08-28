// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cmed/config.hpp -- the settings a requester gets to choose. Deadlines are the application's own;
// socketPath is the daemon's, built from where it bound. MaxDomains and MaxName stay absent --
// they are array lengths and field widths both sides compiled against.

#pragma once

#include <string>

#include "common/timing.hpp"

namespace cmed
{

// Every deadline defaults, so a caller reading no file still runs. socketPath does not: guessing
// it would mean guessing the daemon's deployment layout.
struct CmedClientConfig_t
{
    // ── rendezvous ─────────────────────────────────────────────────
    // The socket the daemon bound. Empty is refused rather than filled in.
    std::string socketPath;

    // How long connect() waits for the Welcome once the socket has accepted. A socket nobody is
    // listening on is refused at once and spends none of this.
    timing::Millis setupTimeout{5000};

    // ── waits ──────────────────────────────────────────────────────
    // Deadlines on waiting for the daemon, not on holding a domain. A caller that holds a domain
    // for a minute is not late; a daemon that has not answered in one is.
    timing::Millis lockTimeout{5000};

    // How long a requester spins on the answer word before it sleeps. Past that it burns this
    // caller's own core, so it is a value a deployment picks.
    timing::Micros spin{10};
};

// Missing file: every field defaults. Present but bad: kvconfig::ParseError for a malformed line,
// CmedInvalidArgumentError for an unusable value. @path is this application's file, @daemonPath the daemon's.
[[nodiscard]] CmedClientConfig_t loadClientConfig(const std::string& path, const std::string& daemonPath);

}  // namespace cmed
