// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// logging.hpp -- per-Event emit() overloads for the stderr trace axis.
// CME_LOGGING gate in logging.cpp: real fprintf(stderr) or empty stubs.

#pragma once

#include "core/types.hpp"
#include "observe/event.hpp"

namespace cme
{
struct LocalPeerState;

namespace logging
{

// Primary fallback: no-op for any Event without a specific overload.
template <observe::Event event, typename... T_Args>
inline void emit(observe::EventTag_t<event>, T_Args&&...) noexcept
{
}

// ── SWOT Recovery FSM events ─────────────────────────────────────
void emit(observe::EventTag_t<observe::Event::RecoveryClaimStarted>,
          LocalPeerState& peerState, PeerId deadPeerId) noexcept;
void emit(observe::EventTag_t<observe::Event::RecoveryClaimed>,
          LocalPeerState& peerState, PeerId deadPeerId) noexcept;
void emit(observe::EventTag_t<observe::Event::RecoveryTakeover>,
          LocalPeerState& peerState, DomainId domainId) noexcept;
void emit(observe::EventTag_t<observe::Event::RecoveryCompleted>,
          LocalPeerState& peerState, PeerId deadPeerId) noexcept;

}  // namespace logging
}  // namespace cme
