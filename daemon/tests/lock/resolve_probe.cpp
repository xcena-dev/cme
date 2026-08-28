// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// resolve_probe.cpp -- a name reaches its own domain and no other.
//
// A name that fills the field carries no terminator, so comparison must stop at the field's end.
// The names here separate a correct comparison from a lenient one: a full field, the same name
// one letter shorter, and a prefix pair. Which domain was reached is read off the state words,
// since the API returns a guard and never exposes the resolved id.

#include <cinttypes>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "cmed/errors.hpp"
#include "cmed/guard.hpp"
#include "cmed/session.hpp"
#include "harness/helper.hpp"
#include "harness/helper_requester.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

// The longest a field takes: one below MaxName, so its terminator still fits.
constexpr std::string_view FullName = "0123456789abcde";

// The same name one letter shorter. It sits at the higher slot on purpose: the scan runs upwards,
// so a comparison that stopped at the shorter name's length would answer with the slot below.
constexpr std::string_view ShortName = "0123456789abcd";

// A prefix pair, ordered the same way. `lane` is `lane0` cut short.
constexpr std::string_view LongLane = "lane0";
constexpr std::string_view ShortLane = "lane";

constexpr std::uint32_t FullSlot = cmed::harness::FirstDataSlot;
constexpr std::uint32_t ShortSlot = FullSlot + 1;
constexpr std::uint32_t LongLaneSlot = FullSlot + 2;
constexpr std::uint32_t ShortLaneSlot = FullSlot + 3;

// Where the moved-name case puts FullName the second time. Empty until then, and inside the scanned
// range so a lookup that reached it would be seen.
constexpr std::uint32_t MovedSlot = FullSlot + 4;

constexpr std::uint32_t PublishedSlots = 5;

// One grant per lock() call the cases below make.
constexpr std::uint32_t ExpectedGrants = 5;

// Every other published domain has to stay at Idle while this one is held. Checking only that the
// wanted domain moved would pass a lookup that raised two.
void reachesOnly(probe::Context& ctx,
                 cmed::CmedSession& session,
                 cmed::protocol::SharedArea_t& area,
                 std::string_view name,
                 std::uint32_t wanted)
{
    const cmed::CmedGuard guard = session.lock(name);
    if (!ctx.checkf(static_cast<bool>(guard), "`%s` locks", std::string{name}.c_str()))
    {
        return;
    }

    std::uint32_t reached = cmed::protocol::NoDomain;
    std::uint32_t moved = 0;
    for (std::uint32_t slot = FullSlot; slot < FullSlot + PublishedSlots; ++slot)
    {
        if (!cmed::harness::isIdle(area, slot))
        {
            reached = slot;
            ++moved;
        }
    }

    ctx.checkf(reached == wanted && moved == 1, "`%s` reaches slot %" PRIu32 " and only that slot",
               std::string{name}.c_str(), wanted);
}

void refuses(probe::Context& ctx, cmed::CmedSession& session, const char* name)
{
    bool threw = false;
    try
    {
        // The cast is what the call is for: this line is expected to throw, not to return a guard.
        static_cast<void>(session.lock(name));
    }
    catch (const cmed::CmedUnknownDomainError&)
    {
        threw = true;
    }
    ctx.checkf(threw, "`%s` is refused as an unknown domain", name);
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"resolve-probe"};
    return probe::run(
        "resolve probe",
        [&scratch](probe::Context& ctx)
        {
            const std::string areaName = scratch.makeAreaName("");
            cmed::harness::ProbeArea area{areaName.c_str()};
            cmed::harness::publishDomains(area.shared(), cmed::harness::FirstDataSlot,
                                          {FullName, ShortName, LongLane, ShortLane});

            const cmed::harness::StubDaemon daemon{area.shared()};
            const cmed::harness::StubSetup setup{scratch.makePath("cmed.sock"), area.descriptor()};
            cmed::CmedSession session = setup.openRequester();

            ctx.openCase("a name reaches its own domain");
            reachesOnly(ctx, session, area.shared(), FullName, FullSlot);
            reachesOnly(ctx, session, area.shared(), ShortName, ShortSlot);
            reachesOnly(ctx, session, area.shared(), LongLane, LongLaneSlot);
            reachesOnly(ctx, session, area.shared(), ShortLane, ShortLaneSlot);

            ctx.openCase("a name that is nobody's");
            // Filling the field, which no write takes, so it names nothing that could be stored.
            refuses(ctx, session, "0123456789abcdef");
            // A prefix of a live name, and a live name with one letter added.
            refuses(ctx, session, "lan");
            refuses(ctx, session, "lane01");

            // A session remembers where a name was, and a slot can be retired and claimed again under
            // another name, so the remembered id is checked against the slot before use.
            ctx.openCase("a name that moved to another slot");
            cmed::harness::resolveSlot(area.shared(), FullSlot).markState(cmed::protocol::DomainState::Free);
            cmed::harness::publishDomain(area.shared(), MovedSlot, FullName);
            reachesOnly(ctx, session, area.shared(), FullName, MovedSlot);

            ctx.openCase("what the daemon was asked for");
            static_cast<void>(daemon.awaitGrants(ExpectedGrants));
            ctx.checkf(daemon.grants() == ExpectedGrants, "%" PRIu32 " grants, one per lock",
                       daemon.grants());

            // One acquire per slot, and the slot each one reached. A total on its own reads the same
            // whether every lock reached its own domain or two of them reached one.
            for (const std::uint32_t slot : {FullSlot, ShortSlot, LongLaneSlot, ShortLaneSlot, MovedSlot})
            {
                ctx.checkf(daemon.grantsFor(slot) == 1, "slot %" PRIu32 " was asked for once, and once only",
                           slot);
            }
        });
}
