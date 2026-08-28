// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// time_probe.cpp -- the two clock types, on their own.
//
// Deadline and Stopwatch are shared types leaned on well beyond any one caller's test of them, so
// the contract itself is asserted here rather than left implicit. The whole design rests on one
// distinction: a spent budget answers nullopt and never a zero duration, since zero is a
// syscall's "do not wait" and not "ran out".

#include <cstdint>
#include <optional>
#include <thread>

#include "common/timing.hpp"
#include "tests/probe_context.hpp"

namespace
{

void aBudgetWithTimeLeftAnswersWithIt(probe::Context& ctx)
{
    ctx.openCase("a deadline with time left");

    constexpr timing::Secs Generous{30};
    const timing::Deadline deadline{Generous};

    const std::optional<timing::Nanos> remaining = deadline.remaining();
    ctx.check(remaining.has_value(), "remaining() is engaged");
    ctx.check(remaining.has_value() && *remaining > timing::Nanos::zero(),
              "and carries a duration above zero");
    ctx.check(remaining.has_value() && *remaining <= Generous, "no larger than the budget it was given");
    ctx.check(!deadline.expired(), "and expired() agrees it has time left");
}

// The case the type exists for. A spent budget is not a zero duration, because a caller passing
// that on would ask a syscall not to wait rather than learning it had run out.
void aSpentBudgetAnswersNulloptAndNotZero(probe::Context& ctx)
{
    ctx.openCase("a deadline that has run out");

    const timing::Deadline deadline{timing::Millis{20}};
    std::this_thread::sleep_for(timing::Millis{40});

    const std::optional<timing::Nanos> remaining = deadline.remaining();
    ctx.check(!remaining.has_value(), "remaining() is nullopt once the budget is spent");
    ctx.check(deadline.expired(), "and expired() agrees");

    // Read across the boundary rather than only past it. A budget that answered zero on the way out
    // would still be engaged here, and asserting nullopt afterwards alone would not see it.
    const timing::Deadline crossing{timing::Millis{15}};
    bool everZero = false;
    while (!crossing.expired())
    {
        const std::optional<timing::Nanos> left = crossing.remaining();
        everZero = everZero || (left.has_value() && *left <= timing::Nanos::zero());
    }
    ctx.check(!everZero, "and no read on the way out ever answers a zero duration");
}

void aZeroBudgetIsSpentOnArrival(probe::Context& ctx)
{
    ctx.openCase("a deadline with no budget at all");

    const timing::Deadline immediate{timing::Nanos::zero()};
    ctx.check(!immediate.remaining().has_value(), "a zero timeout is spent from the start");
    ctx.check(immediate.expired(), "and reads as expired");

    // A caller computing a timeout can arrive at a negative one, and that is the same answer rather
    // than a duration pointing backwards.
    const timing::Deadline overdue{timing::Millis{-5}};
    ctx.check(!overdue.remaining().has_value(), "so is a negative one");
    ctx.check(overdue.expired(), "and it reads as expired too");
}

// A budget that ran out cannot report time left again. Asked repeatedly, it has to keep saying so,
// or a retry loop reading it twice would sleep once more on a deadline it had already passed.
void aSpentBudgetStaysSpent(probe::Context& ctx)
{
    ctx.openCase("a spent deadline, asked again");

    const timing::Deadline deadline{timing::Millis{10}};
    std::this_thread::sleep_for(timing::Millis{25});

    bool everCameBack = false;
    bool everDisagreed = false;
    for (std::uint32_t round = 0; round < 100; ++round)
    {
        const std::optional<timing::Nanos> remaining = deadline.remaining();
        everCameBack = everCameBack || remaining.has_value();
        everDisagreed = everDisagreed || (deadline.expired() == remaining.has_value());
    }

    ctx.check(!everCameBack, "100 reads and none of them reports time left");
    ctx.check(!everDisagreed, "and expired() never disagrees with remaining()");
}

void aStopwatchOnlyMovesForward(probe::Context& ctx)
{
    ctx.openCase("a stopwatch");

    const timing::Stopwatch waited;

    const timing::Nanos first = waited.elapsed();
    ctx.check(first >= timing::Nanos::zero(), "elapsed() is never negative");

    const timing::Nanos second = waited.elapsed();
    ctx.check(second >= first, "and never goes backwards between two reads");

    constexpr timing::Millis Nap{30};
    std::this_thread::sleep_for(Nap);
    ctx.check(waited.elapsed() >= Nap, "a sleep of a known length is covered by what it reports");

    // Two started apart measure different spans, which is what says each carries its own origin
    // rather than reading one shared start.
    const timing::Stopwatch later;
    ctx.check(later.elapsed() < waited.elapsed(), "and one started later reports less than one started earlier");
}

}  // namespace

int main()
{
    return probe::run("time probe",
                      [](probe::Context& ctx)
                      {
                          aBudgetWithTimeLeftAnswersWithIt(ctx);
                          aSpentBudgetAnswersNulloptAndNotZero(ctx);
                          aZeroBudgetIsSpentOnArrival(ctx);
                          aSpentBudgetStaysSpent(ctx);
                          aStopwatchOnlyMovesForward(ctx);
                      });
}
