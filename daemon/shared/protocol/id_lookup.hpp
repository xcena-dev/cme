// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/protocol/id_lookup.hpp -- names to ids, for a caller that would otherwise scan for them.
//
// Knows nothing about what an id names. What is absent is the caller's word to choose, so nothing
// here carries a sentinel. Nothing here locks either: the holder already takes a mutex over it.

#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "shared/protocol/domain_name.hpp"

namespace cmed::protocol
{

class IdLookup
{
public:
    // ── public methods ─────────────────────────────────────────────
    // nullopt for a name this lookup has never been told about, which for a cache means it has yet to
    // be filled in and for an index means nothing carries that name.
    [[nodiscard]] std::optional<std::uint32_t> find(const DomainName& name) const
    {
        const auto found = named_.find(name);
        return found != named_.end() ? std::optional<std::uint32_t>{found->second} : std::nullopt;
    }

    void set(const DomainName& name, std::uint32_t identifier)
    {
        named_[name] = identifier;
    }

    // @identifier and not the name alone: a name being dropped from one id may have been set at
    // another since, and that entry is the live one.
    void clear(const DomainName& name, std::uint32_t identifier)
    {
        const auto found = named_.find(name);
        if (found != named_.end() && found->second == identifier)
        {
            named_.erase(found);
        }
    }

private:
    std::unordered_map<DomainName, std::uint32_t> named_;
};

}  // namespace cmed::protocol
