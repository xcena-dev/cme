// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/protocol/domain_name.hpp -- a name the registry can carry, asked once instead of at each use.
//
// The width is cme's, because the record this name ends up in is cme's. A name that fills the field
// leaves no terminator, and cme refuses one, so the ceiling here is one below the field.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string_view>

#include "cme/limits.hpp"

namespace cmed::protocol
{

// Holds the field's bytes rather than a view of the caller's, so one can be kept past the call that
// made it. Its whole value is that make() has already asked, so a writer taking one cannot fail.
class DomainName
{
public:
    static constexpr std::uint32_t FieldWidth = cme::MaxDomainNameLen;

    // ── factories ──────────────────────────────────────────────────
    // Nothing for a name no slot could hold: empty, or long enough to leave no terminator.
    [[nodiscard]] static std::optional<DomainName> make(std::string_view name) noexcept
    {
        if (name.empty() || name.size() >= FieldWidth)
        {
            return std::nullopt;
        }

        return DomainName{name};
    }

    // ── operator== ─────────────────────────────────────────────────
    // The whole field, padding included, which is how the registry compares two of these.
    [[nodiscard]] bool operator==(const DomainName& other) const noexcept
    {
        return std::memcmp(held_, other.held_, FieldWidth) == 0;
    }

    [[nodiscard]] bool operator!=(const DomainName& other) const noexcept
    {
        return !(*this == other);
    }

    // ── accessors ──────────────────────────────────────────────────
    // The name without its padding, for a caller putting it in a message a person reads.
    [[nodiscard]] std::string_view getText() const noexcept
    {
        return read(held_);
    }

    // What a hash of one is taken over, which is the field and not the text: two fields holding one
    // text are one value, and the padding is what makes that true.
    [[nodiscard]] std::string_view getField() const noexcept
    {
        return std::string_view{held_, FieldWidth};
    }

    // ── the field ──────────────────────────────────────────────────
    // Reads whatever a field holds, because one arrives from a peer and every byte pattern has to
    // read as something. Ends at the first NUL, and an all-zero field reads as no name at all.
    [[nodiscard]] static std::string_view read(const char (&field)[FieldWidth]) noexcept
    {
        const std::string_view stored{field, FieldWidth};
        return stored.substr(0, stored.find('\0'));
    }

    // How a field says it holds no name, which is the one state a write cannot express.
    static void clear(char (&field)[FieldWidth]) noexcept
    {
        std::memset(field, 0, FieldWidth);
    }

    // ── public methods ─────────────────────────────────────────────
    // No answer to give: make() already asked, and what is copied is the whole field.
    void write(char (&field)[FieldWidth]) const noexcept
    {
        std::memcpy(field, held_, FieldWidth);
    }

private:
    // ── ctor / dtor ────────────────────────────────────────────────
    // Writes the whole field, since a field is compared and hashed whole and a stale tail would make
    // one name two values.
    explicit DomainName(std::string_view name) noexcept
    {
        std::memcpy(held_, name.data(), name.size());
        std::memset(held_ + name.size(), 0, FieldWidth - name.size());
    }

    char held_[FieldWidth]{};
};

}  // namespace cmed::protocol

template <>
struct std::hash<cmed::protocol::DomainName>
{
    [[nodiscard]] std::size_t operator()(const cmed::protocol::DomainName& name) const noexcept
    {
        return std::hash<std::string_view>{}(name.getField());
    }
};
