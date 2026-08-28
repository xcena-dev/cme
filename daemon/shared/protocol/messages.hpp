// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/protocol/messages.hpp -- one class per message, each naming its type and reading its payload.
//
// Together rather than a file each, since none is longer than what the base already decided: which
// type it is, and which payload comes back.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "shared/protocol/message.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::protocol
{

// ── the payloads ───────────────────────────────────────────────────────
// In MessageType order, so a reader looking up a type finds its payload in the same place.

// What a request gets back. Zero on success and a negative errno otherwise, the same shape the state
// machine's result word carries, so a requester turns a failure into an error one way only.
struct Answer_t
{
    std::int32_t result;
};

// What the requester compiled against, asked before any descriptor changes hands: refusing here
// costs one message, while handing over a mapping it will reject costs the descriptor for nothing.
struct Hello_t
{
    std::uint32_t abiVersion;
};

// Carries no name. The area travels as the descriptor attached to this message, and a requester that
// received it needs the size to map it and the version to check what it mapped.
struct Welcome_t
{
    std::uint32_t abiVersion;
    std::uint32_t areaBytes;
};

// What all four domain requests carry. The field is the registry's width: a name that fitted a request
// but not a slot would be created in the region and unreachable through this area.
struct DomainRequest_t
{
    char name[MaxName];
};

// What each of them is on the wire. Held here rather than left to the compiler, because both ends read
// a payload as its leading bytes so a later version may append a field and must not move one.
static_assert(sizeof(Answer_t) == 4, "Answer_t is one 32-bit result");
static_assert(sizeof(Hello_t) == 4, "Hello_t is one 32-bit version");
static_assert(sizeof(Welcome_t) == 8, "Welcome_t is a version then a size");
static_assert(offsetof(Welcome_t, areaBytes) == 4, "and the size follows the version");
static_assert(sizeof(DomainRequest_t) == MaxName, "DomainRequest_t is the name and nothing else");

// ── the messages ───────────────────────────────────────────────────────
// One instantiation per fixed-type message: the type and the payload are the whole difference
// between them, so the class carries both and the base does the rest.
template <MessageType kind, typename T_Payload>
class TypedMessage : public Message
{
public:
    explicit TypedMessage(std::string_view bytes) noexcept
        : Message{bytes}
    {
    }

    [[nodiscard]] std::optional<T_Payload> read() const noexcept
    {
        return payload<T_Payload>();
    }

    [[nodiscard]] static std::string frame(std::uint32_t version, std::uint32_t sequence, T_Payload carried)
    {
        return makeFrame(kind, version, sequence, carried);
    }

protected:
    [[nodiscard]] MessageType expects() const noexcept override
    {
        return kind;
    }
};

using HelloMessage = TypedMessage<MessageType::Hello, Hello_t>;
using WelcomeMessage = TypedMessage<MessageType::Welcome, Welcome_t>;
using AnswerMessage = TypedMessage<MessageType::Answer, Answer_t>;

// The one message whose type is not fixed: create, delete, join and leave carry this payload, and the
// caller says which of the four it is willing to read.
class DomainRequestMessage : public Message
{
public:
    DomainRequestMessage(std::string_view bytes, MessageType asked) noexcept
        : Message{bytes},
          asked_{asked}
    {
    }

    [[nodiscard]] std::optional<DomainRequest_t> read() const noexcept
    {
        return payload<DomainRequest_t>();
    }

    // @asked says which of the four this is, since the payload does not.
    [[nodiscard]] static std::string frame(MessageType asked, std::uint32_t version, std::uint32_t sequence,
                                           DomainRequest_t wanted)
    {
        return makeFrame(asked, version, sequence, wanted);
    }

protected:
    [[nodiscard]] MessageType expects() const noexcept override
    {
        return asked_;
    }

private:
    MessageType asked_;
};

}  // namespace cmed::protocol
