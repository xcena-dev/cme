// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// config.hpp -- tunable algorithm constants, grouped by the mechanism that reads them.
//
// Layout ceilings (MaxPeers/MaxDomains) live in core/types.hpp; changing them breaks on-disk layout.

#pragma once

#include <cstdint>

#include "common/timing.hpp"

namespace cme::config
{

// Lock acquire: how long a caller waits, and how it spins before it sleeps.
namespace acquire
{
inline constexpr auto Timeout = timing::Secs{1};
// Must exceed a handoff's arrival ~3x: a miss falls to the ~50us nanosleep floor.
inline constexpr auto SpinWindow = timing::Millis{1};
// PAUSE count per poll, doubling from min to max. Above 128 the gap rivals a handoff.
inline constexpr std::uint32_t SpinPausesMin = 1;
inline constexpr std::uint32_t SpinPausesMax = 1;
// REQUEST: after a timeout drops the demand, how long an in-flight grant may still land.
inline constexpr auto WithdrawSettle = timing::Micros{10};
}  // namespace acquire

// Poll thread: its tick, and the periodic passes it makes. Periods are time, not ticks, because a
// loaded core stretches a tick to milliseconds.
namespace poll_thread
{
inline constexpr auto Interval = timing::Micros{10};
// Per domain: reconcile holder belief with the record, to adopt a handoff written only there.
inline constexpr auto OwnershipCheckInterval = timing::Millis{50};
// Re-read the cached scan scopes: the admission line (new joiners) and the active-domain bitmap.
inline constexpr auto ScanRefreshInterval = timing::Millis{50};
}  // namespace poll_thread

// Membership: the member-slot cache and a clean departure.
namespace membership
{
// Freshness of the read-through member cache. REQUEST demand is read fresh, not from it.
inline constexpr auto CacheTTL = timing::Micros{5};
// A leaving peer stays Leaving this long: CacheTTL plus a granter preempted mid-grant (one timeslice).
inline constexpr auto LeaveDrainWindow = timing::Millis{1};
}  // namespace membership

// Failure detection. Erring long is safe: declaring a live peer dead is the dangerous direction.
namespace liveness
{
inline constexpr auto DeadWindow = timing::Millis{500};
// Declared pairwise clock-skew bound, so a live but skewed peer is never declared dead.
inline constexpr auto ClockSkewBound = timing::Millis{50};
inline constexpr auto DeadWindowEffective = DeadWindow + ClockSkewBound + membership::CacheTTL;
}  // namespace liveness

// Last-writer-wins stakes, shared by the admission lease and the recovery claim.
namespace stake
{
// Recheck delay after staking, sized off the worst observed settle (~1 ms).
inline constexpr auto Settle = timing::Millis{5};
// Longest a staker may take from reading the line to flushing its stake. A slower one gives up.
inline constexpr timing::Millis Window{1};
// Assumed bound on how long a flushed store stays invisible to another host.
inline constexpr timing::Millis StoreVisibilityLag{2};
}  // namespace stake

// Admission lease on the member-slot allocator.
namespace lease
{
// An unchanged nonce this old is presumed dead and stolen. Erring long avoids a double claim.
inline constexpr auto Timeout = timing::Millis{200};
inline constexpr auto PollGap = timing::Millis{2};
// Covers a full MaxPeers burst.
inline constexpr auto AcquireDeadline = timing::Secs{5};
}  // namespace lease

// Recovery of a dead peer's slot and domains.
namespace recovery
{
// Consecutive quiet ticks before FINISH, so a straggler grant naming the dead peer still lands.
inline constexpr std::uint32_t QuietCycles = 100;
// Epochs a seize jumps: one shadow can sit a single epoch above the truth, and the rest is margin.
inline constexpr std::uint64_t SeizeEpochGap = 8;
// Control-lock budget for the orphan sweep. A 0 ns probe never lands under concurrent sweepers.
inline constexpr auto SweepControlTimeout = timing::Millis{2};
// Assumed bound on an authority's stall between re-reading its claim and the write it guards.
inline constexpr timing::Millis TickStall{100};
// A late stake stays this long before withdrawal, since it may have deposed an authority mid-write.
inline constexpr auto LateStakeHold = TickStall + stake::StoreVisibilityLag * 2;
}  // namespace recovery

static_assert(stake::Window + stake::StoreVisibilityLag * 2 <= stake::Settle,
              "a stake inside Window must land before an earlier staker's re-read");
static_assert(recovery::QuietCycles * poll_thread::Interval >= membership::LeaveDrainWindow,
              "the quiet span must cover the departure drain");
static_assert((poll_thread::OwnershipCheckInterval + poll_thread::ScanRefreshInterval) * 4 <= acquire::Timeout,
              "a waiter must see a scan refresh and a reconcile well within one acquire");

}  // namespace cme::config
