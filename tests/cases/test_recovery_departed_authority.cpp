// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_recovery_departed_authority.cpp -- an authority that leaves cleanly in the middle of a recovery
// must not keep its target stranded once a new peer is admitted into the authority's slot.
//
// The leave stops the poll thread mid-takeover, so the target stays Recovering, and a newcomer then
// takes the authority's id. A claim left naming that id would read as a live author.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <thread>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/layout/geometry.hpp"
#include "core/policy/recovery_authority_layout.hpp"
#include "core/types.hpp"
#include "helper.hpp"
#include "observe/failpoint.hpp"
#include "test_context.hpp"
#include "util/coherency.hpp"

namespace test
{
namespace
{

using Status = cme::Geometry::Member_t::Status;

constexpr std::uint32_t MaxDomains = 3;     // control + two data
constexpr cme::PeerId MaxPeers = 2;         // the authority and the doomed peer; the newcomer reuses the first
constexpr const char* HeldLane = "lane0";   // the one the doomed peer takes
constexpr const char* OtherLane = "lane1";  // so a takeover has a loop to stop halfway through
constexpr cme::DomainId HeldDomain = 1;     // created first, so it sits right after control

// The seeder leaves before either child joins, so admission hands out the slots in this order.
constexpr cme::PeerId AuthoritySlot = 0;
constexpr cme::PeerId DoomedSlot = 1;

// The hold is reached after the doomed peer's grace window plus scheduling, never a fixed span.
constexpr std::uint32_t HoldWaitMs = 4000;
constexpr std::uint32_t PeerWaitMs = 4000;
constexpr std::uint32_t ChildWaitMs = 10000;
constexpr std::uint32_t StepMs = 1;
// Far past the one ms drain, so the leave has asked the poll thread to stop before it is let go.
constexpr std::uint32_t StopMarginMs = 50;
// A resume from a stranded claim is one settle and a quiet span once the target is long dead.
constexpr std::uint32_t ResumeWaitMs = 2000;

// One word per signal in a shared scratch buffer, written by one process and read by another.
enum class Signal : std::uint32_t
{
    AuthorityOpen,    // parent: join first
    AuthorityOpened,  // authority: joined, the takeover hold armed
    DoomedOpen,       // parent: join and take the lane
    DoomedHolds,      // doomed: holds the lane
    DoomedDie,        // parent: exit without leaving
    AuthorityHeld,    // authority: stopped inside its takeover of the doomed peer
    AuthorityGone,    // authority: left cleanly, its slot is None
    CaseDone,         // parent: the region has been read, exit
    Count,
};

enum class Role : std::uint32_t
{
    Authority,
    Doomed,
    Count,
};

void raiseSignal(std::uint32_t* words, Signal signal)
{
    __atomic_store_n(&words[static_cast<std::uint32_t>(signal)], 1u, __ATOMIC_RELEASE);
}

[[nodiscard]] bool awaitSignal(const std::uint32_t* words, Signal signal, std::uint32_t deadlineMs)
{
    return harness::waitUntil(
        [words, signal]
        {
            return __atomic_load_n(&words[static_cast<std::uint32_t>(signal)], __ATOMIC_ACQUIRE) != 0;
        },
        deadlineMs, StepMs);
}

[[nodiscard]] cme::PeerId readClaimAuthor(const cme::Geometry& region, cme::PeerId target)
{
    const auto claim = cme::coherency::get(cme::RecoveryAuthorityLayout{region}.getClaim(target),
                                           harness::currentRun().coherency());
    return static_cast<cme::PeerId>(claim.recoveryAuthority);
}

// Leaves from a second thread, because the destructor joins the poll thread parked at the hold, and
// lets that thread go only once the leave has asked it to stop.
void runAuthority(std::uint32_t* words)
{
    if (!awaitSignal(words, Signal::AuthorityOpen, ChildWaitMs))
    {
        return;
    }
    cme::failpoint::hold(cme::failpoint::Boundary::TakeoverMidLoop);
    std::optional<cme::Session> session{harness::openSession()};
    raiseSignal(words, Signal::AuthorityOpened);
    if (!cme::failpoint::awaitHeld(timing::Millis{HoldWaitMs}))
    {
        cme::failpoint::release();
        return;
    }
    raiseSignal(words, Signal::AuthorityHeld);

    std::thread leaver{[&session]
                       {
                           session.reset();
                       }};
    harness::sleepMs(StopMarginMs);
    cme::failpoint::release();
    leaver.join();
    raiseSignal(words, Signal::AuthorityGone);
    static_cast<void>(awaitSignal(words, Signal::CaseDone, ChildWaitMs));
}

// Leaks its session and its Guard, so no leave runs: the slot stays Active and the lane held.
void runDoomed(std::uint32_t* words)
{
    if (!awaitSignal(words, Signal::DoomedOpen, ChildWaitMs))
    {
        return;
    }
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
    auto* session = new cme::Session{harness::openSession()};
    session->joinDomain(HeldLane);
    static_cast<void>(new cme::Guard{session->lock(HeldLane)});
    raiseSignal(words, Signal::DoomedHolds);
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
    static_cast<void>(awaitSignal(words, Signal::DoomedDie, ChildWaitMs));
}

void runRole(Role role, std::uint32_t* words)
{
    switch (role)
    {
        case Role::Authority:
            runAuthority(words);
            return;
        case Role::Doomed:
            runDoomed(words);
            return;
        case Role::Count:
            return;
    }
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    if (!cme::failpoint::Compiled)
    {
        harness::TestContext::skip("built without CME_FAILPOINT, so the authority cannot be held");
    }

    harness::formatSession(MaxDomains, MaxPeers);
    {
        auto seeder = harness::openSession();
        seeder.createDomain(HeldLane);
        seeder.createDomain(OtherLane);
    }
    const auto region = harness::openBoundRegion();
    const auto signals = ctx.scratch<std::uint32_t>("signals", static_cast<std::uint32_t>(Signal::Count));
    if (!ctx.check(static_cast<bool>(signals), "shared signal buffer mapped"))
    {
        return;
    }
    std::uint32_t* words = signals.data();

    // Forked while the parent holds no slot, so the newcomer it opens later is the only other peer.
    const std::uint32_t spawned = harness::spawnChildren(static_cast<std::uint32_t>(Role::Count),
                                                         [words](std::uint32_t index)
                                                         {
                                                             runRole(static_cast<Role>(index), words);
                                                         });

    raiseSignal(words, Signal::AuthorityOpen);
    const bool authorityUp = awaitSignal(words, Signal::AuthorityOpened, PeerWaitMs);
    raiseSignal(words, Signal::DoomedOpen);
    const bool doomedUp = awaitSignal(words, Signal::DoomedHolds, PeerWaitMs);
    if (!ctx.check(authorityUp && doomedUp && harness::hasMemberStatus(region, AuthoritySlot, Status::Active) &&
                       harness::readDomainRecord(region, HeldDomain).getHolder() == DoomedSlot,
                   "the authority and the doomed peer took the expected slots, and the doomed peer holds the lane"))
    {
        raiseSignal(words, Signal::CaseDone);
        harness::reapChildren(spawned);
        return;
    }

    raiseSignal(words, Signal::DoomedDie);
    const bool authorityHeld = awaitSignal(words, Signal::AuthorityHeld, HoldWaitMs);
    const bool authorityGone = awaitSignal(words, Signal::AuthorityGone, PeerWaitMs);
    const bool doomedRecovering = harness::hasMemberStatus(region, DoomedSlot, Status::Recovering);
    const cme::PeerId claimAuthor = readClaimAuthor(region, DoomedSlot);
    const bool authoritySlotFree = harness::hasMemberStatus(region, AuthoritySlot, Status::None);

    auto newcomer = harness::openSession();
    const bool newcomerInAuthoritySlot = harness::hasMemberStatus(region, AuthoritySlot, Status::Active);
    const bool resumed = harness::waitUntil(
        [&region]
        {
            return harness::hasMemberStatus(region, DoomedSlot, Status::None);
        },
        ResumeWaitMs, StepMs);
    std::printf("  doomed slot %s within %u ms of the newcomer\n", resumed ? "freed" : "still Recovering",
                ResumeWaitMs);

    raiseSignal(words, Signal::CaseDone);
    harness::reapChildren(spawned);

    ctx.check(authorityHeld, "the authority seized the doomed peer and stopped inside its takeover");
    ctx.check(authorityGone && authoritySlotFree, "the authority left cleanly and its slot read None");
    ctx.check(doomedRecovering, "the doomed peer's slot was left Recovering by the abandoned recovery");
    ctx.check(cme::isNoPeer(claimAuthor), "the departing authority retracted its claim on the doomed peer");
    ctx.check(newcomerInAuthoritySlot, "the newcomer was admitted into the authority's slot");
    ctx.check(resumed, "the doomed peer's recovery was resumed and its slot freed");
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
