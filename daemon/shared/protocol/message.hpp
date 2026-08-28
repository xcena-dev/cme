// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/protocol/message.hpp -- what every message on the socket has in common: a header in front,
// a payload behind it, and the version both ends have to agree on before either reads further.
//
// Setup and control only. A lock and a release are stores in the shared area, not messages here.
// A derived class names the one type it is and reads its own payload; the base does the rest.

#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace cmed::protocol
{

// ── what a message can be ──────────────────────────────────────────────
// Join and leave are here for the requester that keeps a domain across turns. Without them the daemon
// joins for each turn and leaves with it, which is two region operations per acquire.
enum class MessageType : std::uint32_t
{
    None = 0,

    // A category of one, high up because it will never gain a sibling: it answers whatever was asked.
    Answer = 1,

    // One exchange, so the two stay adjacent. Welcome is what carries the area descriptor.
    Hello = 2,
    Welcome = 3,

    // Last, because this is the group that grows. A new request type appends here and moves nothing.
    CreateDomain = 4,
    DeleteDomain = 5,
    JoinDomain = 6,
    LeaveDomain = 7,
};

// Whether @type is a domain-scoped request rather than setup or control.
[[nodiscard]] constexpr bool isDomainRequest(MessageType type) noexcept
{
    return type == MessageType::CreateDomain || type == MessageType::DeleteDomain ||
           type == MessageType::JoinDomain || type == MessageType::LeaveDomain;
}

// ── the header ─────────────────────────────────────────────────────────
// version travels first and is never conditional on type, so a build reading a version it does not
// know can refuse without agreeing on anything else in the message.
struct Header_t
{
    std::uint32_t version;
    MessageType type;

    // Echoed by the answer, which is what lets a requester tell the answer to its own request from
    // one left in the socket by a request it abandoned.
    std::uint32_t sequence;

    // Travels despite SOCK_SEQPACKET, to tell a short message from one of an unknown type.
    std::uint32_t payloadBytes;
};

// What both ends compile against, not tunables: a receiver sized below a sender loses the descriptors
// the sender believes it handed over. Below Header_t, because one ceiling is measured from it.
enum ProtocolLimits : std::uint32_t
{
    // What this build speaks, and the oldest it still answers. Two constants and not one, because
    // refusing every version but its own is a policy, and a policy needs somewhere to be written.
    Version = 1,
    OldestVersion = 1,

    // A message longer than this is refused rather than cut, since a truncated control buffer is how
    // a descriptor goes missing without either end seeing an error.
    LongestMessage = sizeof(Header_t) + 64,

    MostDescriptors = 4,
};

// Whether this build can answer a peer claiming @version. A range and not equality, so widening what
// is accepted is one constant rather than a search for every place that compared against its own.
[[nodiscard]] constexpr bool isSupported(std::uint32_t version) noexcept
{
    return version >= OldestVersion && version <= Version;
}

// ── one message ────────────────────────────────────────────────────────
class Message
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // Interprets bytes it does not own and copies none of them, so it must not outlive the buffer.
    explicit Message(std::string_view bytes) noexcept
        : bytes_{bytes}
    {
    }

    Message(const Message&) = delete;
    Message(Message&&) = delete;
    virtual ~Message() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────────
    Message& operator=(const Message&) = delete;
    Message& operator=(Message&&) = delete;

    // ── accessors ──────────────────────────────────────────────────────
    // Whether read() can answer: a header is present, this build speaks its version, and the type is
    // the one this class is. False is the caller's only test before it reads anything else.
    [[nodiscard]] bool isReadable() const noexcept
    {
        const std::optional<Header_t> read = header();
        return read && isSupported(read->version) && read->type == expects();
    }

    // The peer's, so an answer framed with it is readable by that peer rather than only by this build.
    [[nodiscard]] std::uint32_t version() const noexcept
    {
        const std::optional<Header_t> read = header();
        return read ? read->version : 0;
    }

    [[nodiscard]] std::uint32_t sequence() const noexcept
    {
        const std::optional<Header_t> read = header();
        return read ? read->sequence : 0;
    }

    [[nodiscard]] MessageType type() const noexcept
    {
        const std::optional<Header_t> read = header();
        return read ? read->type : MessageType::None;
    }

protected:
    // ── the payload half ───────────────────────────────────────────────
    // The bytes behind the header, for the derived class that knows which type they are. A longer
    // payload is read as its leading bytes, which is what lets a later version add a field at the end.
    template <typename T_Payload>
    [[nodiscard]] std::optional<T_Payload> payload() const noexcept
    {
        static_assert(std::is_trivially_copyable_v<T_Payload>, "a protocol payload is copied as bytes");

        const std::optional<Header_t> read = header();
        if (!read || !isReadable() || read->payloadBytes < sizeof(T_Payload) ||
            bytes_.size() < sizeof(Header_t) + sizeof(T_Payload))
        {
            return std::nullopt;
        }

        T_Payload carried{};
        std::memcpy(&carried, bytes_.data() + sizeof(Header_t), sizeof(carried));
        return carried;
    }

    // What this message is. Asked by isReadable(), so a derived class refuses another's bytes for free.
    // The base accepts whatever arrived, which is what lets a dispatcher read the type before choosing.
    [[nodiscard]] virtual MessageType expects() const noexcept
    {
        return type();
    }

    // ── framing ────────────────────────────────────────────────────────
    // The bytes of one message. Static, so framing needs no received message, and @version is the
    // peer's: this build's own would be unreadable by a peer speaking an older one.
    template <typename T_Payload>
    [[nodiscard]] static std::string makeFrame(MessageType type, std::uint32_t version, std::uint32_t sequence,
                                               const T_Payload& payload)
    {
        static_assert(std::is_trivially_copyable_v<T_Payload>, "a protocol payload is copied as bytes");
        static_assert(sizeof(Header_t) + sizeof(T_Payload) <= LongestMessage, "payload outgrows LongestMessage");

        const Header_t header{version, type, sequence, static_cast<std::uint32_t>(sizeof(T_Payload))};

        std::string message(sizeof(header) + sizeof(payload), '\0');
        std::memcpy(message.data(), &header, sizeof(header));
        std::memcpy(message.data() + sizeof(header), &payload, sizeof(payload));
        return message;
    }

private:
    // Copied out rather than read through a pointer into the buffer: no Header_t object lives there,
    // so reinterpreting those bytes as one would be type punning even where the alignment holds.
    [[nodiscard]] std::optional<Header_t> header() const noexcept
    {
        if (bytes_.size() < sizeof(Header_t))
        {
            return std::nullopt;
        }

        Header_t read{};
        std::memcpy(&read, bytes_.data(), sizeof(read));
        return read;
    }

private:
    std::string_view bytes_;
};

}  // namespace cmed::protocol
