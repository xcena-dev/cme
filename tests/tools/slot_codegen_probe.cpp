// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// slot_codegen_probe.cpp -- two exported functions that do nothing but one 64 B slot transfer each.
//
// check_slot_codegen.sh disassembles exactly these symbols. A probe rather than a real call site
// because a slot transfer inlined into a policy is surrounded by that policy's code, and telling
// the two apart in a disassembly is guesswork.

#include <cstdint>

#include "cme/shared.hpp"
#include "util/coherency.hpp"

namespace
{

struct Slot_t
{
    std::uint64_t word[cme::CacheLineBytes / sizeof(std::uint64_t)];
};

static_assert(sizeof(Slot_t) == cme::CacheLineBytes, "the probe must move one whole line");

}  // namespace

extern "C" void cmeProbeSlotSet(void* slot, const void* value) noexcept
{
    cme::coherency::set(static_cast<Slot_t*>(slot), *static_cast<const Slot_t*>(value),
                        cme::CoherencyMode::CacheCoherent);
}

extern "C" void cmeProbeSlotGet(const void* slot, void* out) noexcept
{
    *static_cast<Slot_t*>(out) =
        cme::coherency::get(static_cast<const Slot_t*>(slot), cme::CoherencyMode::CacheCoherent);
}

// Both probes are named here so the linker keeps them. Running this binary proves nothing; the
// check reads its disassembly.
int main()
{
    Slot_t from{};
    Slot_t into{};
    cmeProbeSlotSet(&into, &from);
    cmeProbeSlotGet(&into, &from);
    return 0;
}
