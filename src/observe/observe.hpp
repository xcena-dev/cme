// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// observe.hpp -- unified observability dispatch (header-only).
// OBSERVE_EVENT(Event::X, args...) fans out to all axes via overload set;
// missing overloads fall back to per-axis no-op template. No observe.cpp.

#pragma once

#include <chrono>
#include <utility>

// timing::Nanos is used only inside OBSERVE_CPU_TIME's body, and include-cleaner does not read an
// unexpanded macro, so it reads this header as unused.
#include "common/timing.hpp"  // NOLINT(misc-include-cleaner)
#include "observe/event.hpp"
#include "observe/latency.hpp"  // OBSERVE_LATENCY_BEGIN/END + Latency enum + trace
#include "observe/logging.hpp"
#include "observe/stats.hpp"
#include "util/cpu.hpp"  // NOLINT(misc-include-cleaner) -- OBSERVE_CPU_TIME below

namespace cme::observe
{

// Fan out to every axis; each axis acts or no-ops via overload.
template <Event event, typename... T_Args>
inline void emit(EventTag_t<event> tag, T_Args&&... args) noexcept
{
    stats::emit(tag, std::forward<T_Args>(args)...);
    logging::emit(tag, std::forward<T_Args>(args)...);
}

}  // namespace cme::observe

// OBSERVE_EVENT(Event::Name, args...) -- IDE goto-def lands on the enum entry.
#define OBSERVE_EVENT(EVT, ...)                                             \
    ::cme::observe::emit(::cme::observe::EventTag_t<::cme::observe::EVT>{}, \
                         __VA_ARGS__)

// getThreadCpuTime() is a syscall with no vDSO path, so sampling it per acquire pollutes the
// hot path. Compiled out to zero unless stats or profile -- the only consumers.
#if defined(CME_STATS) || defined(CME_PROFILE)
#define OBSERVE_CPU_TIME() (::cme::cpu::getThreadCpuTime())
#else
#define OBSERVE_CPU_TIME() (::timing::Nanos::zero())
#endif
