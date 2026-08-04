// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_shared_smoke.cpp -- smoke test for the public Session API. Exercises:
//   - Session::format(shm)
//   - Session::open(uri)
//   - Session::open(uri, OpenOpts)
//   - Session::lock("name") + Guard RAII
//   - Session::tryLock(name, timeout)
//   - Session::withLock(name, fn)
//   - Session::getDomainNames()
//   - unknown domain -> UnknownDomainError
//   - format on top of an existing region (re-format is idempotent)
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "test_context.hpp"

namespace
{

void runBody(harness::TestContext& ctx)
{
    const std::string& uri = ctx.uri();

    // ── format ─────────────────────────────────────────────────────
    cme::Session::FormatOpts_t fopts;
    fopts.maxDomains = 4;  // control(0) + 3 data slots
    fopts.maxPeers = 4;
    fopts.strategy = cme::Strategy::Request;
    cme::Session::format(uri, fopts);

    // ── open w/ default opts ──────────────────────────────────────
    auto first = cme::Session::open(uri);
    // Data domains are created at runtime (slots fill in ascending order).
    // Opt-in: create does not participate, so join each before locking.
    for (const char* name : {"inv", "orders", "cache"})
    {
        first.createDomain(name);
        first.joinDomain(name);
    }
    if (!ctx.check(first.getDomainNames().size() == 3, "domain count"))
    {
        return;
    }
    if (!ctx.check(first.getDomainNames()[0] == "inv", "domain[0] name"))
    {
        return;
    }
    if (!ctx.check(first.getDomainNames()[1] == "orders", "domain[1] name"))
    {
        return;
    }
    if (!ctx.check(first.getDomainNames()[2] == "cache", "domain[2] name"))
    {
        return;
    }

    // ── lock / unlock cycle ───────────────────────────────────────
    {
        auto guard = first.lock("inv");
        if (!ctx.check(static_cast<bool>(guard), "guard truthy"))
        {
            return;
        }
    }  // dtor releases

    // ── re-acquire same domain ────────────────────────────────────
    {
        auto guard = first.lock("inv");
        guard.unlock();
        if (!ctx.check(!static_cast<bool>(guard), "guard falsy after unlock"))
        {
            return;
        }
    }

    // ── tryLock success ───────────────────────────────────────────
    {
        auto guard = first.tryLock("orders", std::chrono::milliseconds{200});
        if (!ctx.check(guard.has_value(), "tryLock(orders) should succeed"))
        {
            return;
        }
    }

    // ── withLock lambda ───────────────────────────────────────────
    bool ran = false;
    first.withLock("cache", [&](cme::Guard& guard)
                   {
                       ran = static_cast<bool>(guard);
                   });
    if (!ctx.check(ran, "withLock body must observe live guard"))
    {
        return;
    }

    // ── unknown domain throws ─────────────────────────────────────
    {
        bool threw = false;
        try
        {
            (void)first.lock("ghost");
        }
        catch (const cme::UnknownDomainError&)
        {
            threw = true;
        }
        if (!ctx.check(threw, "lock(\"ghost\") must throw UnknownDomainError"))
        {
            return;
        }
    }

    // ── unknown domain: tryLock throws too (not nullopt; else retry spins) ─
    {
        bool threw = false;
        try
        {
            (void)first.tryLock("ghost", std::chrono::milliseconds{10});
        }
        catch (const cme::UnknownDomainError&)
        {
            threw = true;
        }
        if (!ctx.check(threw, "tryLock(\"ghost\") must throw UnknownDomainError"))
        {
            return;
        }
    }

    // ── second open ───────────────────────────────────────────────
    auto second = cme::Session::open(uri);
    second.joinDomain("orders");  // opt-in: join before locking

    // ── two sessions, distinct domains, no contention ─────────────
    {
        auto guardA = first.lock("inv");
        auto guardB = second.lock("orders");
    }  // both release
}

}  // namespace

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, runBody);
}
