// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cme/limits.hpp -- the ceilings a region is laid out against.
//
// Here rather than beside the types that use them, because a consumer that lays out its own table
// against a cme domain has to agree with these and would otherwise copy the numbers. A copy of a
// ceiling is a copy that can disagree, and nothing in a build says which one the region was formatted
// with.
//
// These mirror the kernel's me_layout.h enum me_limits. That header is the origin; this one is how
// userspace reads it, and the two are checked against each other where the region is formatted.

#pragma once

#include <cstdint>

namespace cme
{

// Peer slots and domain records in one region. Fixed rather than sized per deployment: they are array
// bounds in the region header, so moving one changes every offset after it.
inline constexpr std::uint32_t MaxPeers = 64;
inline constexpr std::uint32_t MaxDomains = 64;

// Bytes a domain name field holds. NUL-padded, and a name that fills the field carries no terminator,
// so a reader stops at the first NUL or at the field's end.
inline constexpr std::uint32_t MaxDomainNameLen = 16;

// One cacheline, and the unit every on-disk record is sized and aligned to. get and set issue
// whole-line transactions, so a record straddling two lines would be two of them and a reader could
// see half of one write.
inline constexpr std::uint32_t CacheLineBytes = 64;

}  // namespace cme
