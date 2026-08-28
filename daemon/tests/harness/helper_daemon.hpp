// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_daemon.hpp -- the daemon's half of the exchange, as much of it as a requester can tell
// apart, and one stub with the answer to an acquire injected.
//
// A grant, a refusal carrying an errno, and no answer at all each drive a different exit from the
// requester, so the policy is a parameter rather than three stubs. No cme::Session and no CXL
// ownership: what a requester can tell apart is the words, and those are all this writes.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>

#include "common/bitmap.hpp"
#include "common/timing.hpp"
#include "harness/helper_area.hpp"
#include "harness/helper_handler.hpp"
#include "shared/protocol/shared_area.hpp"
#include "shared/util/futex.hpp"

namespace cmed::harness
{

// What the stub does with one acquire. There is nothing to decide about a release: a holder publishes
// Idle itself and only rings, so no answer of this stub's is in that path at all.
enum class LockAnswer : std::uint32_t
{
    Grant,   // Granting, result 0, LockHeld
    Refuse,  // Granting, the error below, Error
    Ignore,  // nothing at all: the requester waits out its deadline
};

struct ServeVerdict_t
{
    LockAnswer answer{LockAnswer::Grant};

    // Written into result before Error is published, and negative the way the daemon writes it:
    // the requester negates it back into the exception's code().
    std::int32_t error{0};
};

// Takes the domain so a case can answer one domain differently from another.
using ServePolicy = std::function<ServeVerdict_t(std::uint32_t domainId)>;

[[nodiscard]] inline ServePolicy grantEveryLock()
{
    return [](std::uint32_t)
    {
        return ServeVerdict_t{LockAnswer::Grant, 0};
    };
}

// @error is a negative errno, as the daemon writes it.
[[nodiscard]] inline ServePolicy refuseEveryLock(std::int32_t error)
{
    return [error](std::uint32_t)
    {
        return ServeVerdict_t{LockAnswer::Refuse, error};
    };
}

[[nodiscard]] inline ServePolicy answerNoLock()
{
    return [](std::uint32_t)
    {
        return ServeVerdict_t{LockAnswer::Ignore, 0};
    };
}

class StubDaemon
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // Grants every acquire, which is what a case whose subject is the requester wants.
    explicit StubDaemon(protocol::SharedArea_t& area)
        : StubDaemon{area, grantEveryLock()}
    {
    }

    StubDaemon(protocol::SharedArea_t& area, ServePolicy policy)
        : area_{&area},
          policy_{std::move(policy)},
          worker_{[this]
                  {
                      run();
                  }}
    {
    }

    StubDaemon(const StubDaemon&) = delete;
    StubDaemon(StubDaemon&&) = delete;

    ~StubDaemon()
    {
        running_.store(false, std::memory_order_release);
        util::wakeAllWaiters(resolveDoorbellWord(*area_));
        worker_.join();
    }

    // ── operator= ──────────────────────────────────────────────────
    StubDaemon& operator=(const StubDaemon&) = delete;
    StubDaemon& operator=(StubDaemon&&) = delete;

    // ── accessors ──────────────────────────────────────────────────
    // The totals answer that no exchange was lost or invented anywhere; the per-domain counts answer
    // which slot each one reached, which no state word says once the exchange is over.
    [[nodiscard]] std::uint32_t grants() const noexcept
    {
        return grants_.served();
    }

    [[nodiscard]] std::uint32_t grantsFor(std::uint32_t domainId) const noexcept
    {
        return grants_.servedFor(domainId);
    }

    [[nodiscard]] std::uint32_t refusals() const noexcept
    {
        return refusals_.served();
    }

    [[nodiscard]] std::uint32_t refusalsFor(std::uint32_t domainId) const noexcept
    {
        return refusals_.servedFor(domainId);
    }

    // What makes a release the caller never asked for visible. A guard that gave one turn back twice
    // would ring twice, and nothing in the state words afterwards would say so.
    [[nodiscard]] std::uint32_t releases() const noexcept
    {
        return releases_.served();
    }

    [[nodiscard]] std::uint32_t releasesFor(std::uint32_t domainId) const noexcept
    {
        return releases_.servedFor(domainId);
    }

    // ── settling ───────────────────────────────────────────────────
    // Each count rises after the word a requester woke on, and a release is only a knock nothing waits
    // for. At least @wanted, so a case still asserts the equality and one count too many stays a failure.
    [[nodiscard]] bool awaitGrants(std::uint32_t wanted, timing::Millis within = StubSettleWait) const
    {
        return grants_.awaitServed(wanted, within);
    }

    [[nodiscard]] bool awaitRefusals(std::uint32_t wanted, timing::Millis within = StubSettleWait) const
    {
        return refusals_.awaitServed(wanted, within);
    }

    [[nodiscard]] bool awaitReleases(std::uint32_t wanted, timing::Millis within = StubSettleWait) const
    {
        return releases_.awaitServed(wanted, within);
    }

private:
    // Far above the stub's own turn, because what is waited on is this thread reaching a runnable core
    // under whatever else the suite is running.
    static constexpr timing::Millis StubSettleWait{2000};

    // Short rather than blocking, so one pass ends on its own even with nothing to serve and nobody
    // knocking. What ends the loop is stop(), which clears the flag and knocks.
    static constexpr timing::Millis IdleTurn{5};

    // Its own drain and not a ServeLoop: the daemon's loop hands each domain to a worker, and that
    // pool holds a domain in flight until its worker is done. A case reading this stub's answers wants
    // them as the ring arrives, so this answers on the thread that drained the bit.
    void run()
    {
        while (running_.load(std::memory_order_acquire))
        {
            const std::uint32_t seen = area_->readDoorbell();

            bitmap::Bits<cmed::MaxDomains> taking = area_->domain.pending.takeAll();
            const bool served = !taking.isEmpty();
            while (!taking.isEmpty())
            {
                serve(taking.popLowest());
            }

            // Straight round again when anything was answered, since a ring that arrived while this
            // answered coalesces into one bit and a wait here would let the next one join it.
            if (served)
            {
                continue;
            }

            if (!running_.load(std::memory_order_acquire))
            {
                return;
            }

            static_cast<void>(util::waitOnWord(resolveDoorbellWord(*area_), seen, timing::Nanos{IdleTurn},
                                               timing::Nanos::zero()));
        }
    }

    void serve(std::uint32_t domainId)
    {
        protocol::Domain_t& domain = resolveSlot(*area_, domainId);
        switch (domain.getState())
        {
            case protocol::RequestState::LockRequested:
                answer(domainId, domain, policy_(domainId));
                break;
            case protocol::RequestState::Idle:
                // A holder left with nothing queued behind it, the only reason a domain is rung while
                // idle. The real daemon weighs the turn here; a stub holds none, so this notice is all.
                releases_.record(domainId);
                break;
            default:
                break;
        }
    }

    // result is stored before the settled state, because a requester that sees Error reads result
    // next and would otherwise read whatever the last exchange left there.
    void answer(std::uint32_t domainId, protocol::Domain_t& domain, ServeVerdict_t verdict)
    {
        if (verdict.answer == LockAnswer::Ignore)
        {
            return;
        }

        domain.publish(protocol::RequestState::Granting);
        domain.wakeRequester();
        if (verdict.answer == LockAnswer::Grant)
        {
            domain.publishGrant();
            domain.wakeRequester();
            grants_.record(domainId);
            return;
        }

        domain.publishFailure(verdict.error);
        domain.wakeRequester();
        refusals_.record(domainId);
    }

    protocol::SharedArea_t* area_;
    ServePolicy policy_;

    std::atomic<bool> running_{true};
    DomainRecorder grants_;
    DomainRecorder refusals_;
    DomainRecorder releases_;

    // Last, so every word above is built before the thread that reads them starts.
    std::thread worker_;
};

}  // namespace cmed::harness
