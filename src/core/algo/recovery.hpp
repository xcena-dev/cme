// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// recovery.hpp -- SWOT P1 claim FSM entry (poll-thread RA monitor).
//
// serviceRecovery drives crash recovery from the poll thread, independent of
// acquire traffic. Mechanism primitives stay in ownership_transfer.{hpp,cpp}.

#pragma once

#include "core/runtime/local_peer_state.hpp"

namespace cme::recovery
{

// Step the RA claim FSM one phase per call (watch->claim->takeover->complete).
// No in-call sleep; poll-thread only. See recovery.cpp for phase breakdown.
void serviceRecovery(LocalPeerState& peerState);

}  // namespace cme::recovery
