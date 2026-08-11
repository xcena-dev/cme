// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// logging.cpp -- per-Event emit() overloads for the stderr trace axis.
//
// CME_LOGGING on -> fprintf(stderr, ...); off -> empty stubs. Recovery lines carry a
// monotonic-ns "[t=<ns>] " prefix, so differencing event times recovers per-phase spans.

#include "observe/logging.hpp"

#include "core/types.hpp"
#include "observe/event.hpp"

#if defined(CME_LOGGING)
#include <chrono>
#include <cinttypes>
#include <cstdio>

#include "common/timing.hpp"
#include "core/runtime/local_peer_state.hpp"
#include "util/cpu.hpp"

#endif

namespace cme::logging
{

#if defined(CME_LOGGING)

void emit(observe::EventTag_t<observe::Event::RecoveryClaimStarted>, LocalPeerState& /*peerState*/,
          PeerId deadPeerId) noexcept
{
    std::fprintf(stderr, "[t=%" PRIu64 "] cme: recovery of peer %u claim started\n", timing::monotonic<timing::Nanos>(),
                 deadPeerId);
}

void emit(observe::EventTag_t<observe::Event::RecoveryClaimed>, LocalPeerState& /*peerState*/,
          PeerId deadPeerId) noexcept
{
    std::fprintf(stderr,
                 "[t=%" PRIu64 "] cme: recovery of peer %u claimed (demand scrubbed, recovering)\n",
                 timing::monotonic<timing::Nanos>(), deadPeerId);
}

void emit(observe::EventTag_t<observe::Event::RecoveryTakeover>, LocalPeerState& /*peerState*/,
          DomainId domainId) noexcept
{
    std::fprintf(stderr, "[t=%" PRIu64 "] cme: recovery seized domain %u from a dead holder\n",
                 timing::monotonic<timing::Nanos>(), domainId);
}

void emit(observe::EventTag_t<observe::Event::RecoveryCompleted>, LocalPeerState& /*peerState*/,
          PeerId deadPeerId) noexcept
{
    std::fprintf(stderr, "[t=%" PRIu64 "] cme: recovery of peer %u complete (membership dropped)\n",
                 timing::monotonic<timing::Nanos>(), deadPeerId);
}

#else  // CME_LOGGING undefined -- empty stubs

void emit(observe::EventTag_t<observe::Event::RecoveryClaimStarted>, LocalPeerState&, PeerId) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::RecoveryClaimed>, LocalPeerState&, PeerId) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::RecoveryTakeover>, LocalPeerState&, DomainId) noexcept
{
}
void emit(observe::EventTag_t<observe::Event::RecoveryCompleted>, LocalPeerState&, PeerId) noexcept
{
}

#endif  // CME_LOGGING

}  // namespace cme::logging
