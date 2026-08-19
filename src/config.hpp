// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// config.hpp -- tunable algorithm constants.
//
// Layout ceilings (MaxPeers/MaxDomains) live in core/types.hpp; changing
// them breaks on-disk layout.

#pragma once

#include <cstdint>

#include "common/timing.hpp"

namespace cme
{

// Wall-clock deadline for a single acquire attempt before takeover probe.
inline constexpr auto AcquireTimeout = timing::Secs{1};

// Poll-thread cadence. LocalPeerState seeds its pollInterval from this.
inline constexpr auto DefaultPollInterval = timing::Micros{10};

// Failure-detector window: >> DefaultPollInterval so a live peer ticks
// several times during the window; short enough ownership doesn't rotate far.
inline constexpr auto LivenessProbeInterval = timing::Micros{500};

// Freshness window for the lazy read-through member cache; a hit skips the FAM read.
// REQUEST demand is not cached here -- it is read fresh on the grant scan.
inline constexpr auto MemberCacheTTL = timing::Micros{5};

// Departure drain: ~Peer stays Leaving this long before its sweep. Only the first MemberCacheTTL is
// a bound; the rest covers a granter preempted between its member read and its record write, which
// is what leaves a record on a departed peer. Sized for a scheduler timeslice, not a page fault.
inline constexpr auto LeaveDrainWindow = timing::Millis{1};

// RA monitor: wall-clock silence window before crash declared.
// Conservative: false suspicion of a live peer (SWOT reliable-FD) is the dangerous direction.
inline constexpr auto DeadWindow = timing::Millis{500};
// Declared pairwise clock-skew bound δ (bounded-skew contract). Widens the effective
// window so a live-but-skewed peer is never falsely declared dead.
inline constexpr auto ClockSkewBound = timing::Millis{50};
// Effective dead window: nominal + δ + member-cache staleness, all folded into one margin.
inline constexpr auto DeadWindowEffective = DeadWindow + ClockSkewBound + MemberCacheTTL;

// Consecutive quiet poll cycles before recovery FINISHes. Guards the same window as
// LeaveDrainWindow -- a straggler grant naming the dead peer after the slot reads None strands
// the domain -- so it is sized to match it: 100 cycles x DefaultPollInterval = 1 ms.
inline constexpr std::uint32_t RecoveryCycles = 100;

// Epochs a recovery seize jumps before stamping itself. publishDomainRecord writes the shadow
// before the truth, so one shadow can sit a single epoch above the truth record; 2 would clear
// it and the rest is margin. Epochs only have to increase, so a gap costs nothing.
inline constexpr std::uint64_t RecoverySeizeEpochGap = 8;

// Orphan-sweep control-lock budget. A 0ns probe never lands under concurrent sweepers, which
// clobber each other's interest. Safe: the heartbeat is bumped before the sweep.
inline constexpr auto SweepControlTimeout = timing::Millis{2};

// Poll cycles between ownership-record scrubs (torn-handoff adopt / AdoptFromRecord).
// Per-domain countdown, so the cold record reads spread instead of bursting on one tick.
inline constexpr std::uint32_t OwnershipCheckInterval = 1024;

// Poll cycles between cached-scan-scope refreshes: the admission line (new joiners) and
// the Header activeDomains bitmap. Rare events, so keep both off the per-cycle path.
inline constexpr std::uint32_t PeriodicScanInterval = 1024;

// Hot-spin window before falling back to sleep in waitForOwnership. Must exceed the handoff
// arrival with ~3x margin: a miss drops to the ~50us nanosleep floor and stalls the convoy.
inline constexpr auto SpinWindow = timing::Millis{1};

// REQUEST withdraw settle: after a lock() timeout drops the demand, how long to wait for an
// in-flight grant to land. Reported as Arrived rather than stranding the domain.
inline constexpr auto RequestWithdrawSettle = timing::Micros{10};

// Hot-spin backoff: PAUSE count per poll doubles from min to max as the wait
// drags. Max >128 inflates convoy latency (gap rivals per-hop handoff).
inline constexpr std::uint32_t SpinPausesMin = 1;
inline constexpr std::uint32_t SpinPausesMax = 1;

// LWW recheck window after staking the admission lease. Sized off the worst observed settle
// (~1ms), not the p50; cold-join/crash only, so the slack costs nothing hot.
inline constexpr auto ClaimSettle = timing::Millis{5};
// A held lease whose nonce is unchanged this long is presumed dead and stolen. Conservative:
// a false steal risks a double claim, and the real hold is a few reads plus two flushes.
inline constexpr auto LeaseTimeout = timing::Millis{200};
// Poll gap while waiting on a held lease, to notice release/handoff promptly.
inline constexpr auto LeasePollGap = timing::Millis{2};
// Wall-clock deadline to acquire the lease before failing (covers a full MaxPeers burst).
inline constexpr auto LeaseAcquireDeadline = timing::Secs{5};

}  // namespace cme
