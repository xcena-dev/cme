// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// exchange_probe.cpp -- what one message is answered with, and what the answer leaves recorded.
//
// handshake_probe drives these two through a real control loop, so it sees the answer that goes back
// out. What it cannot see is the ledger behind it: which names this connection is holding, and what
// the daemon gives back when that connection drops without leaving any of them.
//
// A ledger that records a name the answer refused makes the daemon leave a domain this node never
// entered. One that misses a name the answer took leaves the node inside a domain nobody may delete.
//
// A domain request is answered out of the manager, so a case that sends one owns a region and an
// area. One pair per case, since a name one case created would otherwise be the next one's answer.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "daemon/control/exchange.hpp"
#include "daemon/domain/manager.hpp"
#include "daemon/startup/config.hpp"
#include "harness/helper.hpp"
#include "harness/helper_cme_region.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/protocol/domain_name.hpp"
#include "shared/protocol/message.hpp"
#include "shared/protocol/messages.hpp"
#include "shared/protocol/shared_area.hpp"

namespace
{

constexpr std::uint32_t Asked = 11;

// Not this build's own, so an answer stamped with the peer's version is told apart from one stamped
// with the daemon's. Inside the supported range, since a header outside it never reaches an exchange.
constexpr std::uint32_t PeerVersion = cmed::protocol::Version;

// Any size does. What it is for is arriving in the Welcome unchanged.
constexpr std::uint32_t AreaBytes = 8384;

// The control domain plus room for every name a case creates, since a create the region has no slot
// for is answered NoRoom and no case here is about that refusal.
constexpr std::uint32_t RegionSlots = 8;

constexpr std::string_view FirstName = "lane0";
constexpr std::string_view SecondName = "lane1";
constexpr std::string_view ThirdName = "lane2";

// The region, the area and the manager over both. One case's own, so what it created is gone before
// the next builds its own.
class ProbeDomains
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // @label separates one case's shm names from another's, since a name asked for twice comes back
    // the same both times.
    ProbeDomains(cmed::harness::ProbeScratch& scratch, std::string_view label)
        : areaName_{scratch.makeAreaName(label)},
          regionName_{scratch.makeAreaName(std::string{label} + "-region")},
          area_{areaName_.c_str()},
          region_{regionName_.c_str(), RegionSlots},
          domains_{region_.session(), area_.shared(), config_}
    {
    }

    ProbeDomains(const ProbeDomains&) = delete;
    ProbeDomains(ProbeDomains&&) = delete;
    ~ProbeDomains() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────────
    ProbeDomains& operator=(const ProbeDomains&) = delete;
    ProbeDomains& operator=(ProbeDomains&&) = delete;

    // ── accessors ──────────────────────────────────────────────────────
    [[nodiscard]] cmed::daemon::DomainManager& domains() noexcept
    {
        return domains_;
    }

    // Where a create publishes its name, which is the table every requester resolves against.
    [[nodiscard]] cmed::protocol::SharedArea_t& shared() noexcept
    {
        return area_.shared();
    }

private:
    std::string areaName_;
    std::string regionName_;
    cmed::daemon::DaemonConfig_t config_;
    cmed::harness::ProbeArea area_;
    cmed::harness::ProbeRegion region_;
    cmed::daemon::DomainManager domains_;
};

// The reply word as the requester reads it, or nothing when this was no Answer at all. Every case
// below asks for one, so the unpacking stays here.
[[nodiscard]] std::optional<std::int32_t> readAnswerCode(const std::optional<std::string>& answered)
{
    if (!answered)
    {
        return std::nullopt;
    }

    const cmed::protocol::AnswerMessage answering{*answered};
    const std::optional<cmed::protocol::Answer_t> result = answering.read();
    if (!answering.isReadable() || !result)
    {
        return std::nullopt;
    }
    return result->result;
}

// The manager's answers are an enumeration and the reply word is an errno, so a case that compares
// the two says which answer it meant rather than which number.
[[nodiscard]] constexpr std::int32_t asReplyWord(cmed::daemon::DomainAnswer answer) noexcept
{
    return static_cast<std::int32_t>(answer);
}

[[nodiscard]] std::string frameRequest(cmed::protocol::MessageType asked, std::string_view name)
{
    cmed::protocol::DomainRequest_t wanted{};
    if (const auto named = cmed::protocol::DomainName::make(name))
    {
        named->write(wanted.name);
    }
    return cmed::protocol::DomainRequestMessage::frame(asked, PeerVersion, Asked, wanted);
}

// Sends one request through the exchange and hands back what went out, so a case reads the ledger
// rather than repeating the four lines that drive it.
[[nodiscard]] std::optional<std::string> askFor(const cmed::daemon::DomainRequestExchange& requests,
                                                cmed::protocol::MessageType asked, std::string_view name,
                                                cmed::daemon::JoinLedger& joins)
{
    const std::string message = frameRequest(asked, name);
    const cmed::protocol::Message arrived{message};
    return requests.answer(arrived, message, joins);
}

// Asked before answer(), so a loop holding both exchanges picks one without reading a payload. A type
// claimed by neither is what the loop's own unknown-type branch exists for.
void eachExchangeClaimsItsOwnTypes(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("which types each exchange claims");

    const cmed::daemon::HelloExchange greeting{AreaBytes};

    // A domain exchange answers out of a manager, so building one is what it takes to ask this at all.
    ProbeDomains fixture{scratch, "claims"};
    const cmed::daemon::DomainRequestExchange requests{fixture.domains()};

    ctx.check(greeting.answers(cmed::protocol::MessageType::Hello), "the greeting claims a Hello");
    ctx.check(!greeting.answers(cmed::protocol::MessageType::Welcome) &&
                  !greeting.answers(cmed::protocol::MessageType::Answer) &&
                  !greeting.answers(cmed::protocol::MessageType::CreateDomain) &&
                  !greeting.answers(cmed::protocol::MessageType::None),
              "and nothing else, including the Welcome it sends");

    ctx.check(requests.answers(cmed::protocol::MessageType::CreateDomain) &&
                  requests.answers(cmed::protocol::MessageType::DeleteDomain) &&
                  requests.answers(cmed::protocol::MessageType::JoinDomain) &&
                  requests.answers(cmed::protocol::MessageType::LeaveDomain),
              "the domain exchange claims all four requests");
    ctx.check(!requests.answers(cmed::protocol::MessageType::Hello) &&
                  !requests.answers(cmed::protocol::MessageType::Answer) &&
                  !requests.answers(cmed::protocol::MessageType::None),
              "and neither exchange claims a type the other does");
}

// The size the daemon serves, framed in the version the peer claimed rather than in this build's.
// A Welcome stamped with the daemon's own version is one the peer that asked cannot read.
void aHelloIsAnsweredWithTheSizeAndThePeersVersion(probe::Context& ctx)
{
    ctx.openCase("what a Hello is answered with");

    const cmed::daemon::HelloExchange greeting{AreaBytes};
    cmed::daemon::JoinLedger joins;

    const std::string message = cmed::protocol::HelloMessage::frame(PeerVersion, Asked,
                                                                    cmed::protocol::Hello_t{cmed::protocol::AbiVersion});
    const cmed::protocol::Message arrived{message};
    const std::optional<std::string> answered = greeting.answer(arrived, message, joins);

    // Recorded and then tested, rather than testing what check returns: the analyzer cannot tell
    // that a call decided whether the dereference below runs.
    ctx.check(answered.has_value(), "a Hello this build serves is answered");
    if (!answered.has_value())
    {
        return;
    }

    const cmed::protocol::WelcomeMessage welcoming{*answered};
    const std::optional<cmed::protocol::Welcome_t> welcome = welcoming.read();

    ctx.check(welcoming.isReadable(), "with a Welcome");
    ctx.check(welcoming.version() == PeerVersion && welcoming.sequence() == Asked,
              "stamped with the version the peer claimed and the sequence it asked");
    ctx.check(joins.empty(), "and a greeting records nothing, since it joined no domain");

    // Recorded and then tested rather than tested through check's return, since a reader of the
    // accesses below cannot tell that a call decided whether they run.
    ctx.check(welcome.has_value(), "the payload behind the Welcome reads");
    if (!welcome.has_value())
    {
        return;
    }
    ctx.check(welcome->areaBytes == AreaBytes, "carrying the size this exchange was built with");
    ctx.check(welcome->abiVersion == cmed::protocol::AbiVersion, "and the abi this build lays the area out to");
}

// Nothing is the caller's cue to drop the connection, and it is the only refusal a Hello has: a peer
// disagreeing about the abi cannot be told so in a protocol it does not agree about.
void aHelloThisBuildCannotServeIsNotAnswered(probe::Context& ctx)
{
    ctx.openCase("a Hello this build will not answer");

    const cmed::daemon::HelloExchange greeting{AreaBytes};
    cmed::daemon::JoinLedger joins;

    const std::string foreign = cmed::protocol::HelloMessage::frame(PeerVersion, Asked,
                                                                    cmed::protocol::Hello_t{cmed::protocol::AbiVersion + 1});
    const cmed::protocol::Message arrivedForeign{foreign};
    ctx.check(!greeting.answer(arrivedForeign, foreign, joins),
              "an abi this build does not lay out is answered with nothing at all");

    // A frame whose payload did not arrive. Refused here rather than read as zeros, which would be
    // abi zero and would look like a peer claiming a version nobody ever shipped.
    const std::string truncated = cmed::protocol::HelloMessage::frame(PeerVersion, Asked,
                                                                      cmed::protocol::Hello_t{cmed::protocol::AbiVersion})
                                      .substr(0, sizeof(cmed::protocol::Header_t));
    const cmed::protocol::Message arrivedShort{truncated};
    ctx.check(!greeting.answer(arrivedShort, truncated, joins),
              "and so is a Hello whose payload did not arrive");

    // answers() is asked inside answer() as well, so a message routed to the wrong exchange produces
    // no reply rather than a Welcome to something that was not a Hello.
    const std::string request = frameRequest(cmed::protocol::MessageType::CreateDomain, FirstName);
    const cmed::protocol::Message arrivedRequest{request};
    ctx.check(!greeting.answer(arrivedRequest, request, joins),
              "a message this exchange does not claim is not answered by it");
}

// The type is the whole of what tells the four apart, and the answer carries the manager's errno
// rather than the connection being dropped: a peer that got this far speaks the protocol.
void aRequestReachesTheManagerAndTheErrnoComesBack(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("what a domain request is answered with");

    ProbeDomains fixture{scratch, "answered"};
    const cmed::daemon::DomainRequestExchange requests{fixture.domains()};
    cmed::daemon::JoinLedger joins;

    // A delete before any create, so the refusal is the manager's answer about a name no domain on
    // this node carries rather than anything this case arranged.
    const std::optional<std::string> answered =
        askFor(requests, cmed::protocol::MessageType::DeleteDomain, FirstName, joins);

    // Recorded and then tested, for the reason the Hello case above gives.
    ctx.check(answered.has_value(), "the request is answered");
    if (!answered.has_value())
    {
        return;
    }

    const cmed::protocol::AnswerMessage answering{*answered};
    ctx.check(answering.isReadable(), "with an Answer");
    ctx.check(answering.version() == PeerVersion && answering.sequence() == Asked,
              "stamped with the peer's version and the sequence it asked");

    const std::optional<std::int32_t> refused = readAnswerCode(answered);
    ctx.check(refused.has_value(), "whose payload reads");
    if (refused.has_value())
    {
        ctx.checkf(*refused == asReplyWord(cmed::daemon::DomainAnswer::NoSuchDomain),
                   "carrying the errno the manager chose, and it says %d", *refused);
    }

    // The same name under the other type. What tells the two apart is the type alone, so a create
    // answered zero is what says the type reached the manager and not merely the name.
    const std::optional<std::int32_t> created =
        readAnswerCode(askFor(requests, cmed::protocol::MessageType::CreateDomain, FirstName, joins));
    ctx.check(created == asReplyWord(cmed::daemon::DomainAnswer::Served),
              "the same name under CreateDomain is answered zero");
    ctx.check(fixture.shared().resolve(FirstName) != cmed::protocol::NoDomain,
              "and the name is published where a requester resolves it");

    // A request whose payload did not arrive answers nothing and reaches no manager, so a delete
    // this build could not read cannot take the domain the frame named.
    const std::string truncated = frameRequest(cmed::protocol::MessageType::DeleteDomain, FirstName)
                                      .substr(0, sizeof(cmed::protocol::Header_t));
    const cmed::protocol::Message arrivedShort{truncated};
    ctx.check(!requests.answer(arrivedShort, truncated, joins),
              "a request whose payload did not arrive is not answered");
    ctx.check(fixture.shared().resolve(FirstName) != cmed::protocol::NoDomain,
              "and reaches no manager, so the domain it named still stands");
}

// Only what the answer says took. A create the manager refused leaves this connection holding
// nothing, so the departure below it has nothing to give back.
void onlyAnAnswerOfZeroIsRecorded(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("what an answer records");

    ProbeDomains fixture{scratch, "recorded"};
    const cmed::daemon::DomainRequestExchange requests{fixture.domains()};
    cmed::daemon::JoinLedger joins;

    // An empty name is the one create the manager refuses before the region is involved at all.
    const std::optional<std::int32_t> unusable =
        readAnswerCode(askFor(requests, cmed::protocol::MessageType::CreateDomain, "", joins));
    ctx.check(unusable == asReplyWord(cmed::daemon::DomainAnswer::NameUnusable), "an empty name is refused");
    ctx.check(joins.empty(), "and a create the manager refused records nothing");

    static_cast<void>(askFor(requests, cmed::protocol::MessageType::CreateDomain, FirstName, joins));
    ctx.check(joins.count(std::string{FirstName}) == 1, "a create the manager took records the name");

    // Created behind the exchange, so the join below is the first this ledger hears of the name. A
    // join recorded nowhere leaves the node inside a domain no other node may then delete.
    static_cast<void>(fixture.domains().serveCommand(cmed::protocol::MessageType::CreateDomain, SecondName));
    static_cast<void>(askFor(requests, cmed::protocol::MessageType::JoinDomain, SecondName, joins));
    ctx.check(joins.size() == 2 && joins.count(std::string{SecondName}) == 1,
              "and so does a join, which is the other half that puts this node in");

    // The same name twice is one entry, so a departure leaves it once. Leaving it twice would take
    // the node out of a domain another connection on this node is still inside.
    static_cast<void>(askFor(requests, cmed::protocol::MessageType::JoinDomain, FirstName, joins));
    ctx.check(joins.size() == 2, "a name joined twice on one connection is still one entry");
}

// The other half of the sort: a leave or a delete takes the name out, so the departure does not
// leave a domain this connection already left.
void aLeaveOrADeleteTakesTheNameOut(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("what a leave and a delete take out");

    ProbeDomains fixture{scratch, "taken-out"};
    const cmed::daemon::DomainRequestExchange requests{fixture.domains()};
    cmed::daemon::JoinLedger joins;

    static_cast<void>(askFor(requests, cmed::protocol::MessageType::CreateDomain, FirstName, joins));
    static_cast<void>(askFor(requests, cmed::protocol::MessageType::CreateDomain, SecondName, joins));
    if (!ctx.check(joins.size() == 2, "two names are held to begin with"))
    {
        return;
    }

    const std::optional<std::int32_t> left =
        readAnswerCode(askFor(requests, cmed::protocol::MessageType::LeaveDomain, FirstName, joins));
    ctx.check(left == asReplyWord(cmed::daemon::DomainAnswer::Served), "the leave of the first name is served");
    ctx.check(joins.size() == 1 && joins.count(std::string{FirstName}) == 0,
              "a leave the manager took drops that name and leaves the other");

    const std::optional<std::int32_t> removed =
        readAnswerCode(askFor(requests, cmed::protocol::MessageType::DeleteDomain, SecondName, joins));
    ctx.check(removed == asReplyWord(cmed::daemon::DomainAnswer::Served), "the delete of the second is served");
    ctx.check(joins.empty(), "and a delete the manager took drops the last one");

    // Taken out behind the exchange, which is what another connection deleting it looks like from
    // here. The leave that follows is refused for a name this ledger still holds.
    static_cast<void>(askFor(requests, cmed::protocol::MessageType::CreateDomain, FirstName, joins));
    static_cast<void>(fixture.domains().serveCommand(cmed::protocol::MessageType::DeleteDomain, FirstName));

    const std::optional<std::int32_t> refused =
        readAnswerCode(askFor(requests, cmed::protocol::MessageType::LeaveDomain, FirstName, joins));
    ctx.check(refused == asReplyWord(cmed::daemon::DomainAnswer::NoSuchDomain),
              "a leave of a name no domain carries is refused");
    ctx.check(joins.count(std::string{FirstName}) == 1, "a leave the manager refused keeps the name held");
}

// The only signal a requester that died leaves. Unread, the node stays in a domain no other node may
// delete, so what this reaches the manager with is the whole of the recovery.
void aDepartureGivesBackEverythingStillHeld(probe::Context& ctx, cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("what a departure gives back");

    ProbeDomains fixture{scratch, "departure"};
    const cmed::daemon::DomainRequestExchange requests{fixture.domains()};
    cmed::daemon::JoinLedger joins;

    static_cast<void>(askFor(requests, cmed::protocol::MessageType::CreateDomain, FirstName, joins));
    static_cast<void>(askFor(requests, cmed::protocol::MessageType::CreateDomain, SecondName, joins));
    if (!ctx.check(joins.size() == 2, "two names are held to begin with"))
    {
        return;
    }

    requests.giveBack(joins);

    // A delete is the exit for the last participant, so a node outside the domain is refused one.
    // That refusal is what says the leave for this name landed, since nothing else here records one.
    ctx.check(fixture.domains().serveCommand(cmed::protocol::MessageType::DeleteDomain, FirstName) ==
                  cmed::daemon::DomainAnswer::NotParticipating,
              "the departure left the first name");
    ctx.check(fixture.domains().serveCommand(cmed::protocol::MessageType::DeleteDomain, SecondName) ==
                  cmed::daemon::DomainAnswer::NotParticipating,
              "and the second, so it reached one leave per name the connection still held");

    // Nothing held is nothing given back, which is the ordinary path: a peer that left tidily has an
    // empty ledger, and a departure that leaves anything for it would leave a domain twice.
    cmed::daemon::JoinLedger another;
    static_cast<void>(askFor(requests, cmed::protocol::MessageType::CreateDomain, ThirdName, another));

    const cmed::daemon::JoinLedger nothing;
    requests.giveBack(nothing);

    ctx.check(fixture.domains().serveCommand(cmed::protocol::MessageType::DeleteDomain, ThirdName) ==
                  cmed::daemon::DomainAnswer::Served,
              "and a connection holding nothing gives nothing back, so another connection's name stands");
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"exchange-probe"};
    return probe::run("exchange probe",
                      [&scratch](probe::Context& ctx)
                      {
                          eachExchangeClaimsItsOwnTypes(ctx, scratch);
                          aHelloIsAnsweredWithTheSizeAndThePeersVersion(ctx);
                          aHelloThisBuildCannotServeIsNotAnswered(ctx);
                          aRequestReachesTheManagerAndTheErrnoComesBack(ctx, scratch);
                          onlyAnAnswerOfZeroIsRecorded(ctx, scratch);
                          aLeaveOrADeleteTakesTheNameOut(ctx, scratch);
                          aDepartureGivesBackEverythingStillHeld(ctx, scratch);
                      });
}
