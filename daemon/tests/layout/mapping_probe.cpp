// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// mapping_probe.cpp -- the two types that own a mapping, held to the paths a happy run misses.
//
// sync_probe.cpp does this for RobustLock. SharedMapping and CmedArea carry the same bug shape and
// nothing checks it: a destructor that must run exactly once, and a move that must leave the source
// owning nothing, neither visible in a run where every mapping is made and dropped in one scope. A
// double unmap is not reliably a crash, so each case asserts something positive instead.

#include <fcntl.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "cmed/errors.hpp"
#include "daemon/startup/served_area.hpp"
#include "harness/helper.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/area.hpp"
#include "shared/posix/mapping.hpp"
#include "shared/posix/mem_file.hpp"
#include "shared/posix/unique_fd.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

constexpr std::uint64_t MappingBytes = 4096;

// Recognisable in a hexdump, and nothing a zeroed page could hold by accident.
constexpr std::uint64_t Sentinel = 0x0C0FFEE0C0FFEE0DULL;
constexpr std::uint64_t SecondSentinel = 0x0BADF00D0BADF00DULL;

constexpr std::string_view DomainName = "lane0";
constexpr std::uint32_t DomainSlot = cmed::harness::FirstDataSlot;

// Two words a fresh area could not hold by accident, so the area a case is reading is the one it
// wrote and not another of the right size.
constexpr std::int32_t Witness = 42;
constexpr std::uint32_t Knocks = 7;

[[nodiscard]] std::uint64_t& resolveFirstWord(const posix::SharedMapping& mapping) noexcept
{
    return *static_cast<std::uint64_t*>(mapping.base());
}

// The source must not unmap at its own scope exit: the read happens after that scope ends, so a
// source that did unmap would leave the destination pointing into a range already given back.
void moveLeavesTheSourceOwningNothing(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("SharedMapping move");

    const std::string moveLabel = scratch.makeAreaName("move");
    const auto file = posix::MemFile::create(moveLabel, MappingBytes);
    auto destination = posix::SharedMapping::mmap(file.descriptor(), MappingBytes);
    resolveFirstWord(destination) = SecondSentinel;

    {
        auto source = posix::SharedMapping::mmap(file.descriptor(), MappingBytes);
        resolveFirstWord(source) = Sentinel;

        const posix::SharedMapping moved{std::move(source)};

        // Reading the moved-from mapping is the assertion, so both checks say so rather than the
        // analyser being told once per line.
        // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        ctx.check(source.base() == nullptr, "the moved-from mapping holds no address");
        ctx.check(source.bytes() == 0, "and no size");
        // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        ctx.check(moved.base() != nullptr && resolveFirstWord(moved) == Sentinel,
                  "the destination reads what the source wrote");

        // Assignment over a mapping this scope already holds: the old one goes, the new one stays.
        destination = posix::SharedMapping::mmap(file.descriptor(), MappingBytes);
    }

    ctx.check(destination.base() != nullptr && resolveFirstWord(destination) == Sentinel,
              "and the mapping still reads after every source has gone");
}

// CmedArea is a mapping plus a checked header, and its move is the compiler's. What that has to keep
// true is the same: the area reads after the object it was moved out of is gone.
void anAreaOutlivesTheObjectItCameFrom(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("CmedArea move");

    const std::string areaMoveLabel = scratch.makeAreaName("areamove");
    cmed::harness::ProbeArea origin{areaMoveLabel.c_str()};
    cmed::harness::publishDomain(origin.shared(), DomainSlot, DomainName);

    cmed::CmedArea destination = cmed::harness::attachAgain(origin.descriptor());
    {
        cmed::CmedArea source = cmed::harness::attachAgain(origin.descriptor());
        cmed::harness::setResult(cmed::harness::resolveSlot(source.shared(), DomainSlot), Witness);

        destination = std::move(source);
    }

    ctx.check(destination.shared().getAbiVersion() == cmed::protocol::AbiVersion,
              "the moved-to area still carries its header");
    ctx.check(cmed::harness::resolveSlot(destination.shared(), DomainSlot).getFailureCode() == Witness,
              "and still reads the word written through the source");
}

// What every requester rests on once the daemon is gone: the memory is the mapping's, not the
// descriptor's. A daemon that exits closes its own, and nothing about that unmaps anybody.
void aMappingOutlivesTheLastDescriptor(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a mapping whose descriptor is closed");

    posix::SharedMapping orphan;
    {
        const std::string orphanLabel = scratch.makeAreaName("orphan");
        const auto file = posix::MemFile::create(orphanLabel, MappingBytes);
        orphan = posix::SharedMapping::mmap(file.descriptor(), MappingBytes);
        resolveFirstWord(orphan) = Sentinel;
    }

    ctx.check(resolveFirstWord(orphan) == Sentinel, "the mapping still reads after the last descriptor closes");
    resolveFirstWord(orphan) = SecondSentinel;
    ctx.check(resolveFirstWord(orphan) == SecondSentinel, "and still takes a write");
}

// Two formats are two files, so there is no name to collide on: a second daemon cannot reach the
// first one's area at all.
void everyFormatIsItsOwnArea(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("formatArea twice");

    const std::string firstMkfsLabel = scratch.makeAreaName("mkfs-first");
    const std::string secondMkfsLabel = scratch.makeAreaName("mkfs-second");
    cmed::CmedArea first = cmed::daemon::formatArea(firstMkfsLabel);
    cmed::harness::publishDomain(first.shared(), DomainSlot, DomainName);
    cmed::harness::setResult(cmed::harness::resolveSlot(first.shared(), DomainSlot), Witness);
    cmed::harness::setDoorbell(first.shared(), Knocks);

    ctx.check(cmed::harness::resolveSlot(first.shared(), DomainSlot).isExpectedState(cmed::protocol::DomainState::Live),
              "the first format carries a live domain");

    const cmed::CmedArea second = cmed::daemon::formatArea(secondMkfsLabel);

    ctx.check(second.shared().getAbiVersion() == cmed::protocol::AbiVersion,
              "the second stamps its own header");
    ctx.check(!cmed::harness::resolveSlot(second.shared(), DomainSlot).isExpectedState(cmed::protocol::DomainState::Live),
              "and carries none of the first's registry");
    ctx.check(cmed::harness::resolveSlot(second.shared(), DomainSlot).getFailureCode() == 0,
              "nor the word it left in the state machine");
    ctx.check(cmed::harness::getDoorbell(second.shared()) == 0, "and its doorbell starts at zero");
    ctx.check(cmed::harness::resolveSlot(second.shared(), DomainSlot).getName().empty(),
              "and its registry name is empty");

    // The other direction, which is the part a shared name would have broken: writing one area is
    // not writing the other.
    ctx.check(cmed::harness::resolveSlot(first.shared(), DomainSlot).getFailureCode() == Witness,
              "while the first still holds what it wrote");
}

// adopt takes a mapping without checking its header, which is the whole difference between it and
// attach. A version attach would refuse is what tells the two apart.
void adoptTakesAHeaderMapFromWouldRefuse(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("CmedArea::adopt");

    const std::string adoptLabel = scratch.makeAreaName("adopt");
    cmed::harness::ProbeArea area{adoptLabel.c_str()};
    cmed::harness::setAbiVersion(area.shared(), cmed::protocol::AbiVersion + 1);

    bool refused = false;
    try
    {
        // The cast is what the call is for: this line is expected to throw, not to return an area.
        static_cast<void>(cmed::harness::attachAgain(area.descriptor()));
    }
    catch (const cmed::CmedAreaInvalidError&)
    {
        refused = true;
    }
    ctx.check(refused, "attach refuses the stamped-over version");

    posix::UniqueFd copied{::fcntl(area.descriptor(), F_DUPFD_CLOEXEC, 0)};
    auto received = posix::MemFile::adopt(std::move(copied));
    auto* const raw = static_cast<cmed::protocol::SharedArea_t*>(received.base());
    const auto adopted = cmed::CmedArea::adopt(std::move(received), raw);

    ctx.check(adopted.shared().getAbiVersion() == cmed::protocol::AbiVersion + 1,
              "and adopt takes the same mapping without looking");
    ctx.check(cmed::harness::getMaxDomains(adopted.shared()) == cmed::MaxDomains,
              "reading the rest of the header as it stands");
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"mapping-probe"};
    return probe::run("mapping probe",
                      [&scratch](probe::Context& ctx)
                      {
                          moveLeavesTheSourceOwningNothing(ctx, scratch);
                          anAreaOutlivesTheObjectItCameFrom(ctx, scratch);
                          aMappingOutlivesTheLastDescriptor(ctx, scratch);
                          everyFormatIsItsOwnArea(ctx, scratch);
                          adoptTakesAHeaderMapFromWouldRefuse(ctx, scratch);
                      });
}
