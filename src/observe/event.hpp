// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// event.hpp -- Event enum + EventTag_t dispatcher (split from observe.hpp to avoid circular deps).

#pragma once

#include <cstdint>

namespace cme::observe
{

// One value per call site. Missing overload falls back to per-axis no-op.
enum class Event : std::uint16_t
{
    // Ownership lifecycle
    OwnershipRequested,          // acquire() called
    OwnershipAlreadyHave,        // this peer already owner
    OwnershipArrived,            // wait completed (Spin or Sleep)
    OwnershipNotArrived,         // deadline -- proceed to recovery
    OwnershipTransferable,       // use phase ended, transfer barrier lifted
    OwnershipTransferOnRelease,  // outbound transfer on release path
    OwnershipTransferOnPoll,     // outbound transfer on poll path

    // Recovery FSM outcomes
    RecoveryClaimStarted,  // detection passed: RA begins claiming the dead peer (recovery-work t0)
    RecoveryClaimed,       // RA armed: claim confirmed + dead peer's demand scrubbed
    RecoveryTakeover,      // seized one domain from a dead holder
    RecoveryCompleted,     // dead peer's membership purged (recovery-work end)

    NumEvents
};

// Phantom tag for overload dispatch by Event value.
template <Event event>
struct EventTag_t
{
};

}  // namespace cme::observe
