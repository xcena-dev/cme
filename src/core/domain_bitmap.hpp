// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// domain_bitmap.hpp -- the domain-sized bitmap, and the word-count constants sized from it.
// The bitmap itself is bitmap::Bits, which is not domain-specific; what belongs here is the one
// bit count cme uses it with. The on-disk participation / demand arrays are sized by these too.

#pragma once

#include <cstdint>

#include "common/bitmap.hpp"
#include "core/types.hpp"

namespace cme
{

// A packed set of domain ids: load a slot's words into one, operate, store the words back to publish.
using DomainBitmap = bitmap::Bits<MaxDomains>;

// Bits in one bitmap word (the backing word type is uint64).
inline constexpr std::uint32_t DomainBitsPerWord = bitmap::BitsPerWord;

// Words needed for all domains; sizes both in-memory bitmap and on-disk array.
inline constexpr std::uint32_t DomainWordCount = DomainBitmap::WordCount;

}  // namespace cme
