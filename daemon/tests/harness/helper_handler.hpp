// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_handler.hpp -- one count per domain, and what a case asks of those counts.
//
// For a case whose subject calls back once per domain, where a per-probe counter would be a per-probe
// chance to miss the second call for one of them. Drags in helper_area.hpp for the domain ceiling and
// the poll step, and nothing else: no loop and no stub.

#pragma once

#include <array>
#include <cstdint>

#include "common/ordered.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "harness/helper_area.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::harness
{

// One count per domain and one total, so a case asserts both which domains were served and that no
// other was. Every word is atomic: a loop calls the handler on a thread of its own.
class DomainRecorder
{
public:
    // ── public methods ─────────────────────────────────────────────
    // An id past the table lands in the total and nowhere else, so a case still sees that it arrived.
    void record(std::uint32_t domainId) noexcept
    {
        if (domainId < MaxDomains)
        {
            perDomain_[domainId].add(1);
        }
        served_.add(1);
    }

    // For a case that measures a second round on one recorder, where a running total would hide
    // whether the second round served anything at all.
    void forget() noexcept
    {
        for (ordered::Published<std::uint32_t>& count : perDomain_)
        {
            count.store(0);
        }
        served_.store(0);
    }

    // ── settling ───────────────────────────────────────────────────
    // At least @wanted, so a case still asserts the equality and one call too many stays a failure.
    [[nodiscard]] bool awaitServed(std::uint32_t wanted, timing::Millis within = ProbeAttachWait) const
    {
        return poll::waitUntil(
            [this, wanted]
            {
                return served() >= wanted;
            },
            within, ProbePoll);
    }

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] std::uint32_t served() const noexcept
    {
        return served_.load();
    }

    [[nodiscard]] std::uint32_t servedFor(std::uint32_t domainId) const noexcept
    {
        return domainId < MaxDomains ? perDomain_[domainId].load() : 0;
    }

    // One bit per domain the handler saw, for a case whose assertion is that no other domain did.
    [[nodiscard]] std::uint64_t idsSeen() const noexcept
    {
        std::uint64_t seen = 0;
        for (std::uint32_t domainId = 0; domainId < MaxDomains; ++domainId)
        {
            if (servedFor(domainId) != 0)
            {
                seen |= std::uint64_t{1} << domainId;
            }
        }
        return seen;
    }

private:
    std::array<ordered::Published<std::uint32_t>, MaxDomains> perDomain_{};
    ordered::Published<std::uint32_t> served_{0};
};

}  // namespace cmed::harness
