// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_util.hpp -- what a case needs that has nothing to do with cme: randomness, a timestamped
// log line, waiting on a predicate, catching one exception type, and one summary statistic.
//
// Nothing here includes a cme header or test_context.hpp, which is the point of the split: a bench
// or a future tool can take these without taking a Session with them.
//
// In namespace harness with the rest of the shared test code, which is also what keeps `log` from
// sharing the global namespace with ::log(double) from <math.h>.

#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "common/timing.hpp"

namespace harness
{

// xorshift64. Per-thread state, so the sweeps stay independent without sharing.
inline std::uint64_t nextRandom(std::uint64_t& state) noexcept
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Fisher-Yates over a thread's visit order. A permutation, so every element is still visited
// exactly once per sweep -- only the lockstep between threads is broken. Templated on the element
// so this file stays free of cme types; every caller passes domain ids.
template <typename T_Id>
inline void shuffleVisitOrder(std::vector<T_Id>& visit, std::uint64_t& rng) noexcept
{
    for (auto i = static_cast<std::uint32_t>(visit.size()); i > 1; --i)
    {
        std::swap(visit[i - 1], visit[nextRandom(rng) % i]);
    }
}

// Where the timestamps on log lines are measured from. Started at static init so a case that never
// calls startLogClock() still prints a sane elapsed time rather than time since the epoch.
inline timing::Stopwatch& logOrigin()
{
    static timing::Stopwatch origin;
    return origin;
}

// Re-base the clock on the moment a run actually starts, past the harness setup.
inline void startLogClock()
{
    logOrigin().restart();
}

// A timestamped line. A recovery case is a timeline, and the interesting question is
// usually how long after the freeze something happened, so the elapsed seconds lead.
inline void log(const char* fmt, ...)
{
    std::printf("[t=%6.2fs] ", logOrigin().elapsed<timing::SecsF>().count());

    va_list args;
    va_start(args, fmt);
    // The va_list checker loses the va_start above when this header is analysed through another one.
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    std::vprintf(fmt, args);
    va_end(args);

    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Sleep in the unit every test here thinks in. A chrono literal in the middle of a recovery
// sequence reads as noise; the number is the part that matters.
inline void sleepMs(std::uint32_t milliSeconds)
{
    std::this_thread::sleep_for(timing::Millis{milliSeconds});
}

// How often waitUntil re-checks. Long enough that the polling is not itself the load, short
// enough that a transition lands in the right frame.
inline constexpr std::uint32_t PollStepMs = 200;

// Poll @pred until it holds or @deadlineMs elapses; returns whether it held.
//
// A test waits on state another peer publishes, and a fixed sleep long enough to be safe is
// also long enough to hide the timing under test. Polling to a deadline keeps the wait as
// short as the system allows and still bounds it. A clock and not a count of sleeps, because
// pred() costs time too and counting only the sleeps overruns the deadline by that much.
template <typename T_Pred>
[[nodiscard]] bool waitUntil(T_Pred pred, std::uint32_t deadlineMs,
                             std::uint32_t stepMs = PollStepMs)
{
    const timing::Deadline deadline{timing::Millis{deadlineMs}};
    while (!deadline.expired())
    {
        if (pred())
        {
            return true;
        }
        sleepMs(stepMs);
    }
    return pred();
}

// Poll @pred for @windowMs and report whether it held the whole time; false the moment it does not.
//
// waitUntil's twin, and the one a case needs to assert that nothing happened: that a live claim was
// left alone, or a slot stayed occupied. Sampling once proves only that the change had not landed
// yet, so the window is the assertion. The clock matters more here than in waitUntil: over-waiting
// there only gives a transition more room to land, while here it asserts over a longer window than
// the one written.
template <typename T_Pred>
[[nodiscard]] bool holdsFor(T_Pred pred, std::uint32_t windowMs)
{
    const timing::Deadline window{timing::Millis{windowMs}};
    while (!window.expired())
    {
        if (!pred())
        {
            return false;
        }
        sleepMs(PollStepMs);
    }
    return pred();
}

// Threads that join when the group goes out of scope.
//
// The destructor is the reason this exists rather than a vector. A thread that outlives the frame
// it captured reads freed stack, and writing the spawn loop and the join loop apart is what lets
// that happen: an early return between them, or a throw, skips the join. Here the scope is the
// join, so neither can.
//
// spawn adds to the group rather than replacing it, so a case that wants two different bodies
// running together calls it twice and still has one thing to wait on.
class ThreadGroup
{
public:
    ThreadGroup() = default;
    ThreadGroup(const ThreadGroup&) = delete;
    ThreadGroup& operator=(const ThreadGroup&) = delete;
    ThreadGroup(ThreadGroup&&) noexcept = default;
    ThreadGroup& operator=(ThreadGroup&&) noexcept = default;

    ~ThreadGroup()
    {
        join();
    }

    // Start @count threads on @body, which takes the worker index: the peer id or the thread
    // number at every call site.
    template <typename T_Body>
    void spawn(std::uint32_t count, T_Body body)
    {
        threads_.reserve(threads_.size() + count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            threads_.emplace_back(body, index);
        }
    }

    // Wait for them here rather than at the end of the scope, for a case that reads what they
    // wrote before the scope ends. Idempotent, so the destructor then finds nothing left.
    void join()
    {
        for (auto& thread : threads_)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        threads_.clear();
    }

private:
    std::vector<std::thread> threads_;
};

// The whole of a case's parallel section: start @count threads and wait for all of them, with
// nothing in between. A case that has work to do while they run takes a ThreadGroup instead.
template <typename T_Body>
void runThreads(std::uint32_t count, T_Body body)
{
    ThreadGroup group;
    group.spawn(count, body);
}

// Whether @body threw T_Exc. Any other exception propagates rather than being reported as
// "did not throw", so a case that throws the wrong type fails where it happened.
template <typename T_Exc, typename T_Body>
[[nodiscard]] bool threw(T_Body body)
{
    try
    {
        body();
    }
    catch (const T_Exc&)
    {
        return true;
    }
    return false;
}

// Value at @fraction of a sample the caller has already sorted.
[[nodiscard]] inline double
percentile(const std::vector<std::uint64_t>& sorted, double fraction)
{
    if (sorted.empty())
    {
        return 0.0;
    }
    const auto index = static_cast<std::uint64_t>(fraction * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[index]);
}

}  // namespace harness
