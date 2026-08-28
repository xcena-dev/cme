// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/startup/served_area.hpp -- coming up as the daemon of one area: the region this node is a peer
// of, a fresh area on it, and the liveness mutex. What config says is
// next door; this is what a daemon does with it. Lives in a library only the daemon links, so a
// requester that calls in here fails to link rather than zeroing a live area at run time.

#pragma once

#include <string_view>

#include "cme/shared.hpp"
#include "cmed/robust_lock.hpp"
#include "daemon/startup/config.hpp"
#include "shared/area.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::daemon
{

// ── one step of it, for a caller that wants no more than that ──────

// mkfs and nothing else: initialises every robust mutex, stamps the header, then publishes abiVersion
// last. No mode is asked for, since an anonymous file has no path for a permission to sit on.
[[nodiscard]] CmedArea formatArea(std::string_view label);

// ── the four of them, in the one order that works ──────────────────

// Neither copyable nor movable: the liveness mutex lives inside the area this object maps, so one
// object has to be the only thing that unlocks it.
class ServedArea
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // Opens the region, lays down the area, and takes liveness. A step that throws
    // releases the steps before it in reverse, so a failed start leaves nothing published.
    explicit ServedArea(const DaemonConfig_t& config);

    ServedArea(const ServedArea&) = delete;
    ServedArea(ServedArea&&) = delete;
    ~ServedArea() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────
    ServedArea& operator=(const ServedArea&) = delete;
    ServedArea& operator=(ServedArea&&) = delete;

    // ── accessors ──────────────────────────────────────────────────
    // The one cme::Session on this node. Every domain call the daemon makes goes through it.
    [[nodiscard]] cme::Session& session() noexcept
    {
        return session_;
    }

    [[nodiscard]] CmedArea& area() noexcept
    {
        return area_;
    }

    [[nodiscard]] protocol::SharedArea_t& shared() noexcept
    {
        return area_.shared();
    }

private:
    // Declared in the order they are taken, which is also the order the destructor undoes: liveness is
    // released while the mapping it points into is still there.
    cme::Session session_;
    CmedArea area_;
    util::RobustLock alive_;
};

}  // namespace cmed::daemon
