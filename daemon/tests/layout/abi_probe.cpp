// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// abi_probe.cpp -- the shared layout, checked rather than trusted.
//
// A requester and the daemon reach shared/protocol/shared_area.hpp as separate programs, and a size or offset either
// one gets differently is a silently misread shared area. This probe asserts the relations the
// layout rests on, then exercises the bitmap helpers so a signature that stops compiling is caught
// here rather than in the first caller.

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

#include "common/bitmap.hpp"
#include "harness/helper_area.hpp"
#include "shared/protocol/shared_area.hpp"

// offsetof requires a standard-layout type, and a private access specifier is one edit away from
// costing that.
static_assert(std::is_standard_layout_v<cmed::protocol::Domain_t>, "the domain stays standard layout");
static_assert(std::is_standard_layout_v<cmed::protocol::SharedArea_t>, "the area stays standard layout");

static_assert(alignof(cmed::protocol::Domain_t) == cmed::CachelineBytes, "domain is line aligned");
static_assert(sizeof(cmed::protocol::SharedArea_t) % cmed::CachelineBytes == 0, "area is a whole number of lines");

// The width the field carries between two separately built programs, which is what a name crossing
// the wire is measured against.
static_assert(cmed::MaxName == 16, "the name field is sixteen bytes wide");

// The layout is only shared if every member sits where the other program put it. Offsets are
// asserted rather than printed, so a divergence fails the build instead of a reader's attention.
static_assert(offsetof(cmed::protocol::SharedArea_t, domain.pending) % alignof(std::uint64_t) == 0,
              "bitmap words are naturally aligned");
static_assert(sizeof(bitmap::AtomicBits<cmed::MaxDomains>) ==
                  bitmap::AtomicBits<cmed::MaxDomains>::WordCount * sizeof(std::uint64_t),
              "the bitmap is its words and nothing else");
static_assert(alignof(bitmap::AtomicBits<cmed::MaxDomains>) == alignof(std::uint64_t),
              "and carries no alignment of its own");

namespace
{

cmed::protocol::SharedArea_t g_area;

// Set raises exactly its own bit and clear takes exactly that one back down, checked against a mask
// spelled out here rather than reused.
bool bitsRoundTrip()
{
    const std::uint32_t domainId = 5;
    const std::uint32_t bitsPerWord = bitmap::BitsPerWord;
    const std::uint64_t bit = std::uint64_t{1} << (domainId % bitsPerWord);
    const std::uint32_t wordIndex = domainId / bitsPerWord;

    if (!cmed::harness::setPendingBit(g_area, domainId) ||
        cmed::harness::getPendingWord(g_area, wordIndex) != bit)
    {
        return false;
    }
    return cmed::harness::clearPendingBit(g_area, domainId) &&
           cmed::harness::getPendingWord(g_area, wordIndex) == 0;
}

bool statesRoundTrip()
{
    constexpr std::uint32_t Slot = 5;
    cmed::protocol::Domain_t& context = cmed::harness::resolveSlot(g_area, Slot);

    cmed::harness::setState(context, cmed::protocol::RequestState::LockHeld);
    return context.getState() == cmed::protocol::RequestState::LockHeld;
}

}  // namespace

int main()
{
    // Static storage, so the area starts zeroed.
    cmed::harness::setAbiVersion(g_area, cmed::protocol::AbiVersion);

    std::printf("abi=%" PRIu32 " area=%zu domain=%zu\n",
                static_cast<std::uint32_t>(cmed::protocol::AbiVersion),
                sizeof(cmed::protocol::SharedArea_t),
                sizeof(cmed::protocol::Domain_t));

    return (bitsRoundTrip() && statesRoundTrip()) ? 0 : 1;
}
