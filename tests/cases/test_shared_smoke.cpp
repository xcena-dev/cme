// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_shared_smoke.cpp -- smoke test for the public Session API. Exercises:
//   - Session::format(shm)
//   - Session::open(uri) where the medium is cache-coherent, Session::open(uri, OpenOpts) elsewhere
//   - Session::lock("name") + Guard RAII
//   - Session::tryLock(name, timeout)
//   - Session::withLock(name, fn)
//   - Session::getDomainNames()
//   - unknown domain -> UnknownDomainError
//   - format on top of an existing region (re-format is idempotent)
//
// Backend from --backend: uc (a file on an uncacheable mount), dax (a devdax slot), or shm.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "helper.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

// The three data domains this case creates, in the order it creates them. getDomainNames is asked
// for membership, so the order here is only the order of the creates.
constexpr const char* DataDomains[] = {"inv", "orders", "cache"};

// The one withLock runs on, named because the rival has to join it and probe it by the same name.
constexpr const char* WithLockDomain = "cache";

// What the rival waits while the guard is meant to be excluding it, and what it waits once the
// guard is gone. The second is the grant path's budget rather than a refusal window.
constexpr timing::Millis ProbeWindow{50};
constexpr timing::Millis GrantWindow{3'000};

}  // namespace

void runBody(harness::TestContext& ctx)
{
    const std::string& uri = ctx.uri();

    // ── format ─────────────────────────────────────────────────────
    // 4 = control(0) + 3 data slots.
    harness::formatSession(4, 4);

    // ── open ──────────────────────────────────────────────────────
    auto first = harness::openSession();
    // Data domains are created at runtime, and createDomain joins its creator, so nothing here
    // joins them again.
    for (const char* name : DataDomains)
    {
        first.createDomain(name);
    }
    if (!ctx.check(first.getDomainNames().size() == 3, "domain count"))
    {
        return;
    }
    // Membership and count, not position. getDomainNames promises the live data domains and
    // promises nothing about their order, so a change in how the slots are scanned is free to
    // reorder them.
    for (const char* name : DataDomains)
    {
        if (!ctx.checkf(harness::listsDomain(first, name), "getDomainNames lists %s", name))
        {
            return;
        }
    }

    // A second peer, for the two questions this session cannot answer about its own lock: whether
    // a live guard excludes anyone, and whether withLock gave the domain back.
    auto rival = harness::openSession();
    rival.joinDomain(WithLockDomain);

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
        auto guard = first.tryLock("orders", timing::Millis{200});
        if (!ctx.check(guard.has_value(), "tryLock(orders) should succeed"))
        {
            return;
        }
    }

    // ── withLock lambda ───────────────────────────────────────────
    // The body's own flag says only that the callback ran. What withLock owes its caller is the
    // release afterwards, and only another peer's acquire can see that, so the rival probes from
    // inside the body and again once withLock has returned.
    bool ran = false;
    bool intruded = false;
    first.withLock(WithLockDomain, [&](cme::Guard& guard)
                   {
                       ran = static_cast<bool>(guard);
                       intruded = harness::canLock(rival, WithLockDomain, ProbeWindow);
                   });
    if (!ctx.check(ran, "withLock body must observe live guard"))
    {
        return;
    }
    if (!ctx.check(!intruded, "no second peer takes the domain inside the withLock body"))
    {
        return;
    }
    if (!ctx.check(harness::canLock(rival, WithLockDomain, GrantWindow),
                   "withLock gives the domain back, so the rival takes it afterwards"))
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
            (void)first.tryLock("ghost", timing::Millis{10});
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

    // ── second open, through the one-argument overload ────────────
    // That overload fills OpenOpts_t with its defaults, and coherency defaults there to
    // CacheCoherent. So it runs only where the medium is cache-coherent. On the other two the
    // default would be the wrong barrier discipline, and the open would say nothing about the
    // medium the variant exists to cover.
    auto second = (ctx.coherency() == cme::CoherencyMode::CacheCoherent)
                      ? cme::Session::open(uri)
                      : harness::openSession();
    second.joinDomain("orders");  // opt-in: join before locking

    // ── two sessions, distinct domains, no contention ─────────────
    {
        auto guardA = first.lock("inv");
        auto guardB = second.lock("orders");
    }  // both release
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
