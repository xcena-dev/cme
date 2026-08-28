// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// message_probe.cpp -- the header in front of every message, and what it lets a reader read.
//
// handshake_probe drives this through a real control loop, so it reaches the frames a daemon
// answers. What it cannot reach is a frame nothing would send: one shorter than its own header, one
// whose header states a payload longer than the bytes behind it, one of a type this reader is not.
//
// Each of those is a read past a buffer if it gets through, and there is no second check downstream.

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "shared/protocol/message.hpp"
#include "shared/protocol/messages.hpp"
#include "tests/probe_context.hpp"

namespace
{

// Any value does. What it is for is coming back unchanged.
constexpr std::uint32_t Asked = 11;

constexpr std::uint32_t SomeAbi = 3;

// The version an answer is stamped with is the peer's, so a case needs one this build does not
// speak but a peer might.
constexpr std::uint32_t OtherVersion = cmed::protocol::Version + 7;

// A message the base class reads, which accepts whatever type arrived. That is what the control
// loop uses to pick an exchange before any of them has claimed the bytes.
class AnyMessage : public cmed::protocol::Message
{
public:
    explicit AnyMessage(std::string_view bytes) noexcept
        : Message{bytes}
    {
    }
};

// A header with nothing behind it, and a payloadBytes the caller states rather than the truth. What
// a reader has to survive is exactly the case where those two disagree.
[[nodiscard]] std::string headerOnly(std::uint32_t version, cmed::protocol::MessageType type,
                                     std::uint32_t claimedPayloadBytes)
{
    const cmed::protocol::Header_t header{version, type, Asked, claimedPayloadBytes};

    std::string message(sizeof(header), '\0');
    std::memcpy(message.data(), &header, sizeof(header));
    return message;
}

// The eight names against the one predicate a dispatcher branches on. A type sorted into the wrong
// half reaches an exchange that will read a payload it does not carry.
void theFourDomainRequestsAreTheOnesTheHandlerSees(probe::Context& ctx)
{
    ctx.openCase("which types are a domain request");

    ctx.check(cmed::protocol::isDomainRequest(cmed::protocol::MessageType::CreateDomain) &&
                  cmed::protocol::isDomainRequest(cmed::protocol::MessageType::DeleteDomain) &&
                  cmed::protocol::isDomainRequest(cmed::protocol::MessageType::JoinDomain) &&
                  cmed::protocol::isDomainRequest(cmed::protocol::MessageType::LeaveDomain),
              "all four domain requests are one");
    ctx.check(!cmed::protocol::isDomainRequest(cmed::protocol::MessageType::None) &&
                  !cmed::protocol::isDomainRequest(cmed::protocol::MessageType::Answer) &&
                  !cmed::protocol::isDomainRequest(cmed::protocol::MessageType::Hello) &&
                  !cmed::protocol::isDomainRequest(cmed::protocol::MessageType::Welcome),
              "and none of the other four is");

    // The enum's own numbering, which the header states the four grow from. A type inserted before
    // CreateDomain rather than after LeaveDomain would move the four and this check would say so.
    ctx.check(static_cast<std::uint32_t>(cmed::protocol::MessageType::CreateDomain) == 4 &&
                  static_cast<std::uint32_t>(cmed::protocol::MessageType::LeaveDomain) == 7,
              "the four sit at the end of the enumeration, where a new type appends");
}

// A range and not equality, so the two constants are what a widening changes. Both edges, because
// accepting one past either end is a peer this build cannot answer.
void theVersionsThisBuildAnswers(probe::Context& ctx)
{
    ctx.openCase("the version range");

    ctx.check(cmed::protocol::isSupported(cmed::protocol::Version) &&
                  cmed::protocol::isSupported(cmed::protocol::OldestVersion),
              "both ends of the range are supported");
    ctx.check(!cmed::protocol::isSupported(cmed::protocol::Version + 1),
              "one past the newest is not");
    ctx.check(!cmed::protocol::isSupported(cmed::protocol::OldestVersion - 1),
              "and neither is one below the oldest");
    ctx.check(!cmed::protocol::isSupported(0), "zero is not a version, so it is not supported either");
}

// Every byte count below sizeof(Header_t). A reader that copied the header out of a shorter buffer
// would read past its end, and nothing after this point tests the length again.
void aFrameShorterThanItsHeaderAnswersNothing(probe::Context& ctx)
{
    ctx.openCase("a frame shorter than its own header");

    const std::string whole = cmed::protocol::HelloMessage::frame(cmed::protocol::Version, Asked,
                                                                  cmed::protocol::Hello_t{SomeAbi});

    bool everyLengthRefused = true;
    for (std::uint32_t kept = 0; kept < sizeof(cmed::protocol::Header_t); ++kept)
    {
        const std::string cut = whole.substr(0, kept);
        const cmed::protocol::HelloMessage reading{cut};

        everyLengthRefused = everyLengthRefused && !reading.isReadable() && !reading.read().has_value() &&
                             reading.version() == 0 && reading.sequence() == 0 &&
                             reading.type() == cmed::protocol::MessageType::None;
    }

    ctx.checkf(everyLengthRefused, "every length from 0 to %zu answers nothing at all",
               sizeof(cmed::protocol::Header_t) - 1);

    // None is the type an absent header reads as, and it is also a name in the enumeration. A
    // dispatcher branching on the type alone would reach an exchange for it.
    const cmed::protocol::HelloMessage empty{std::string_view{}};
    ctx.check(empty.type() == cmed::protocol::MessageType::None && !empty.isReadable(),
              "so a caller has to ask isReadable and not only the type");
}

// The header says how long the payload is and the buffer says how much arrived. Both are checked,
// because a sender can be wrong about either one.
void aPayloadTheHeaderAndTheBufferDisagreeAbout(probe::Context& ctx)
{
    ctx.openCase("a payload the header and the buffer disagree about");

    // A header claiming a Hello's payload with nothing behind it. The claim is right for the type and
    // the buffer is not, which is the frame a truncating transport would produce.
    const std::string claimed = headerOnly(cmed::protocol::Version, cmed::protocol::MessageType::Hello,
                                           sizeof(cmed::protocol::Hello_t));
    const cmed::protocol::HelloMessage claiming{claimed};
    ctx.check(claiming.isReadable(), "a header alone is readable, since the header is all isReadable sees");
    ctx.check(!claiming.read().has_value(), "but the payload behind it is not there, so read answers nothing");

    // The other direction: the bytes are there and the header understates them. Refused, because the
    // count is the sender's statement of what it meant to send.
    std::string understated = cmed::protocol::HelloMessage::frame(cmed::protocol::Version, Asked,
                                                                  cmed::protocol::Hello_t{SomeAbi});
    cmed::protocol::Header_t shrunk{};
    std::memcpy(&shrunk, understated.data(), sizeof(shrunk));
    shrunk.payloadBytes = sizeof(cmed::protocol::Hello_t) - 1;
    std::memcpy(understated.data(), &shrunk, sizeof(shrunk));

    ctx.check(!cmed::protocol::HelloMessage{understated}.read().has_value(),
              "a payloadBytes under the type's own size answers nothing");

    // What lets a later version append a field: the leading bytes are read and the rest is left.
    std::string overstated = cmed::protocol::HelloMessage::frame(cmed::protocol::Version, Asked,
                                                                 cmed::protocol::Hello_t{SomeAbi});
    cmed::protocol::Header_t grown{};
    std::memcpy(&grown, overstated.data(), sizeof(grown));
    grown.payloadBytes = sizeof(cmed::protocol::Hello_t) + 4;
    std::memcpy(overstated.data(), &grown, sizeof(grown));
    overstated.append(4, '\xEE');

    const std::optional<cmed::protocol::Hello_t> longer = cmed::protocol::HelloMessage{overstated}.read();
    ctx.check(longer && longer->abiVersion == SomeAbi,
              "a longer payload is read as its leading bytes, which is what lets a field be appended");
}

// A derived class refuses another's bytes for free, because expects() is asked by isReadable. The
// base accepts whatever arrived, which is the one reader that has to.
void aReaderRefusesATypeItIsNot(probe::Context& ctx)
{
    ctx.openCase("a message read as the wrong type");

    const std::string welcome = cmed::protocol::WelcomeMessage::frame(
        cmed::protocol::Version, Asked, cmed::protocol::Welcome_t{SomeAbi, 4096});

    const cmed::protocol::AnswerMessage asAnswer{welcome};
    ctx.check(!asAnswer.isReadable(), "a Welcome read as an Answer is not readable");
    ctx.check(!asAnswer.read().has_value(), "so its payload is not handed over as an Answer_t");
    ctx.check(asAnswer.type() == cmed::protocol::MessageType::Welcome,
              "though the type it really is still reads back");

    const AnyMessage asAnything{welcome};
    ctx.check(asAnything.isReadable() && asAnything.type() == cmed::protocol::MessageType::Welcome,
              "the base accepts it, which is how a dispatcher reads the type before choosing");

    // A type no name in the enumeration carries. The base accepts it, so the loop that reads a type
    // before choosing has to have a branch for one it does not know. The analyzer warning below is
    // reporting exactly that.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto beyond = static_cast<cmed::protocol::MessageType>(4096);
    const std::string unknown = headerOnly(cmed::protocol::Version, beyond, 0);
    ctx.check(AnyMessage{unknown}.isReadable(),
              "and it accepts a type no name in the enumeration carries");
}

// The one message whose type is not fixed. The payload does not say which of the four it is, so the
// caller does, and a caller asking for the wrong one must not read it.
void oneDomainRequestReadAsAnother(probe::Context& ctx)
{
    ctx.openCase("a domain request read as another of the four");

    cmed::protocol::DomainRequest_t wanted{};
    wanted.name[0] = 'a';

    const std::string joining = cmed::protocol::DomainRequestMessage::frame(
        cmed::protocol::MessageType::JoinDomain, cmed::protocol::Version, Asked, wanted);

    ctx.check(cmed::protocol::DomainRequestMessage{joining, cmed::protocol::MessageType::JoinDomain}.read().has_value(),
              "the type it was framed as reads it");

    bool everyOtherRefused = true;
    for (const cmed::protocol::MessageType asked :
         {cmed::protocol::MessageType::CreateDomain, cmed::protocol::MessageType::DeleteDomain,
          cmed::protocol::MessageType::LeaveDomain})
    {
        const cmed::protocol::DomainRequestMessage reading{joining, asked};
        everyOtherRefused = everyOtherRefused && !reading.isReadable() && !reading.read().has_value();
    }
    ctx.check(everyOtherRefused, "and none of the other three does");
}

// The version an answer carries is the peer's, not this build's. Stamping its own would frame a
// reply the peer that asked cannot read, which is the failure a version range exists to avoid.
void aFrameCarriesTheVersionItWasGiven(probe::Context& ctx)
{
    ctx.openCase("the version a frame is stamped with");

    const std::string answered = cmed::protocol::AnswerMessage::frame(OtherVersion, Asked,
                                                                      cmed::protocol::Answer_t{-1});
    const cmed::protocol::AnswerMessage reading{answered};

    ctx.check(reading.version() == OtherVersion, "the frame carries the version it was handed");
    ctx.check(reading.sequence() == Asked, "and echoes the sequence unchanged");
    ctx.check(!reading.isReadable(), "this build will not read it back, since that version is not one it speaks");

    // The payload is still there and still that type; only the version stands between the two. So a
    // build that widened its range would read this frame and nothing else about it would change.
    ctx.check(reading.type() == cmed::protocol::MessageType::Answer,
              "though the type and the sequence are readable without agreeing on the version");
}

// A frame is a header and a payload laid out in that order, and both ends compile against these
// sizes rather than negotiating them. LongestMessage is what sizes every receive buffer.
void whatOneFrameMeasures(probe::Context& ctx)
{
    ctx.openCase("the sizes both ends compile against");

    const std::string hello = cmed::protocol::HelloMessage::frame(cmed::protocol::Version, Asked,
                                                                  cmed::protocol::Hello_t{SomeAbi});
    ctx.check(hello.size() == sizeof(cmed::protocol::Header_t) + sizeof(cmed::protocol::Hello_t),
              "a frame is its header and its payload and nothing else");

    const cmed::protocol::DomainRequest_t wanted{};
    const std::string request = cmed::protocol::DomainRequestMessage::frame(
        cmed::protocol::MessageType::CreateDomain, cmed::protocol::Version, Asked, wanted);
    ctx.checkf(request.size() <= cmed::protocol::LongestMessage,
               "the longest frame this build sends is %zu bytes, inside the %u a receiver reads into",
               request.size(), static_cast<std::uint32_t>(cmed::protocol::LongestMessage));

    // The frame a receiver has to be able to take whole. A ceiling below it would refuse the
    // largest message this build sends, and the refusal is a dropped connection.
    ctx.check(sizeof(cmed::protocol::Header_t) + sizeof(cmed::protocol::DomainRequest_t) <=
                  cmed::protocol::LongestMessage,
              "and the largest payload of the four fits it with the header in front");
}

}  // namespace

int main()
{
    return probe::run("message probe",
                      [](probe::Context& ctx)
                      {
                          theFourDomainRequestsAreTheOnesTheHandlerSees(ctx);
                          theVersionsThisBuildAnswers(ctx);
                          aFrameShorterThanItsHeaderAnswersNothing(ctx);
                          aPayloadTheHeaderAndTheBufferDisagreeAbout(ctx);
                          aReaderRefusesATypeItIsNot(ctx);
                          oneDomainRequestReadAsAnother(ctx);
                          aFrameCarriesTheVersionItWasGiven(ctx);
                          whatOneFrameMeasures(ctx);
                      });
}
