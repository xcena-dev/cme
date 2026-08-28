// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// handshake_probe.cpp -- the exchange that hands a requester its area, both ends running.
//
// listener_probe covers admission and fdpass_probe covers a descriptor crossing a socket. This is where
// a message decides whether the area is handed over at all, the first place a real requester and a real
// control loop meet. A peer that got the protocol wrong is refused by the connection closing, since it
// cannot be told so in a protocol it does not speak, and those cases check the empty receive. A domain
// request is refused with an errno in the Answer, which a peer that got this far can read.

#include <sys/stat.h>

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "cmed/errors.hpp"
#include "cmed/guard.hpp"
#include "cmed/session.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "daemon/control/handler.hpp"
#include "daemon/domain/manager.hpp"
#include "daemon/observe/counters.hpp"
#include "daemon/startup/config.hpp"
#include "harness/helper.hpp"
#include "harness/helper_cme_region.hpp"
#include "harness/helper_scratch.hpp"
#include "shared/area.hpp"
#include "shared/posix/seqpacket_socket.hpp"
#include "shared/posix/unique_fd.hpp"
#include "shared/protocol/domain_name.hpp"
#include "shared/protocol/message.hpp"
#include "shared/protocol/messages.hpp"
#include "shared/protocol/shared_area.hpp"
#include "shared/protocol/socket_path.hpp"

namespace
{

constexpr mode_t SocketMode = 0660;

constexpr std::string_view DomainName = "lane0";
constexpr std::uint32_t DomainSlot = cmed::harness::FirstDataSlot;

// The control domain, the name above, and one slot per name a case creates. A create the region has
// no slot for is answered NoRoom, and no case here is about that refusal.
constexpr std::uint32_t RegionSlots = 8;

// One name per case that creates one, since a domain a case left behind is one the next case's own
// answers would have to work around.
constexpr std::string_view MadeName = "lane-made";
constexpr std::string_view AbsentName = "lane-absent";
constexpr std::string_view EarlyName = "lane-early";
constexpr std::string_view HeldFirstName = "lane-held0";
constexpr std::string_view HeldSecondName = "lane-held1";

// Written through the descriptor the daemon handed over and read back through the fixture's own
// mapping. Nothing an exchange would leave in that word, so finding it there is the mapping.
constexpr std::int32_t Witness = 77;

// Short, because the loop reaches the top of every turn to see the stop and a case waits for that.
constexpr timing::Millis IdleTurn{5};

// Long enough that a correct run never reaches it, short enough that a broken one is a red probe
// rather than a hung suite.
constexpr timing::Millis AnswerWithin{500};
constexpr timing::Millis CounterWithin{2000};

// The sequence a case sends. Any value does, because what it is for is coming back unchanged.
constexpr std::uint32_t Asked = 11;

// A ControlHandler turning on its own thread. The destructor stops and joins it, because a thread
// destroyed without a join ends the process and would take the whole probe with it.
class ServingDaemon
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // @domains is the node's one manager, which every case shares: what a control request changes is
    // the domain table, and two managers over one area would each answer out of their own.
    ServingDaemon(const cmed::daemon::DaemonConfig_t& config, posix::FileDesc areaDescriptor,
                  std::uint32_t areaBytes, cmed::daemon::DomainManager& domains)
        : loop_{config, areaDescriptor, areaBytes, domains},
          dispatcher_{[this]
                      {
                          loop_.run();
                      }}
    {
    }

    ServingDaemon(const ServingDaemon&) = delete;
    ServingDaemon(ServingDaemon&&) = delete;

    ~ServingDaemon()
    {
        loop_.stop();
        dispatcher_.join();
    }

    // ── operator= ──────────────────────────────────────────────────────
    ServingDaemon& operator=(const ServingDaemon&) = delete;
    ServingDaemon& operator=(ServingDaemon&&) = delete;

    // ── accessors ──────────────────────────────────────────────────────
    [[nodiscard]] const cmed::daemon::ControlHandler& loop() const noexcept
    {
        return loop_;
    }

private:
    cmed::daemon::ControlHandler loop_;
    std::thread dispatcher_;
};

// The loop counts on its own thread, so a case that read a counter straight after sending would read
// it before the loop had looked. Waits for the value rather than sleeping a guessed amount.
template <typename T_Read>
[[nodiscard]] bool countReaches(T_Read read, std::uint64_t wanted)
{
    return poll::waitUntil([&read, wanted]
                           {
                               return read() >= wanted;
                           },
                           CounterWithin, cmed::harness::ProbePoll);
}

[[nodiscard]] std::optional<posix::SeqpacketSocket::Received_t> ask(posix::SeqpacketSocket& asking,
                                                                    std::string_view message)
{
    if (!asking.send(message, {}))
    {
        return std::nullopt;
    }

    return asking.receive(cmed::protocol::LongestMessage, cmed::protocol::MostDescriptors, AnswerWithin);
}

// The Hello every case but one sends first, since a request before it is refused whatever it asks.
[[nodiscard]] bool greet(posix::SeqpacketSocket& asking)
{
    return ask(asking, cmed::protocol::HelloMessage::frame(cmed::protocol::Version, Asked,
                                                           cmed::protocol::Hello_t{cmed::protocol::AbiVersion}))
        .has_value();
}

// The reply word one request came back with, or nothing when the daemon answered nothing at all. The
// requester reads that word as a negative errno, which is the whole of what a refusal tells it.
[[nodiscard]] std::optional<std::int32_t> askAbout(posix::SeqpacketSocket& asking,
                                                   cmed::protocol::MessageType type, std::uint32_t sequence,
                                                   std::string_view name)
{
    cmed::protocol::DomainRequest_t wanted{};
    if (const auto named = cmed::protocol::DomainName::make(name))
    {
        named->write(wanted.name);
    }

    const std::optional<posix::SeqpacketSocket::Received_t> answered =
        ask(asking, cmed::protocol::DomainRequestMessage::frame(type, cmed::protocol::Version, sequence, wanted));
    if (!answered)
    {
        return std::nullopt;
    }

    const cmed::protocol::AnswerMessage answering{answered->message};
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

// A header this build would not send. Written by hand, because frame() stamps the version it speaks
// and what this case needs is one it does not.
[[nodiscard]] std::string headerOnly(std::uint32_t version, cmed::protocol::MessageType type)
{
    const cmed::protocol::Header_t header{version, type, Asked, 0};

    std::string message(sizeof(header), '\0');
    std::memcpy(message.data(), &header, sizeof(header));
    return message;
}

// The rule for joining the two halves. Both ends compose the path from the same pair, so a requester
// that joined them differently would connect to a name nobody bound.
void theSocketPathBothEndsCompose(probe::Context& ctx)
{
    ctx.openCase("the socket path both ends compose");

    ctx.check(cmed::protocol::buildSocketPath("/run/cmed", "lane") == "/run/cmed/lane.sock",
              "a directory and an area name join with one separator and the suffix");
    ctx.check(cmed::protocol::buildSocketPath("run", "lane") == "run/lane.sock",
              "a relative directory stays relative");
}

void aHelloIsAnsweredWithTheArea(probe::Context& ctx, cmed::harness::ProbeArea& area,
                                 const cmed::daemon::DaemonConfig_t& config, cmed::daemon::DomainManager& domains)
{
    ctx.openCase("Hello");

    const ServingDaemon serving{config, area.descriptor(), area.shared().getAreaBytes(), domains};
    auto asking = posix::SeqpacketSocket::connect(config.socketPath());

    std::optional<posix::SeqpacketSocket::Received_t> answered =
        ask(asking, cmed::protocol::HelloMessage::frame(cmed::protocol::Version, Asked, cmed::protocol::Hello_t{cmed::protocol::AbiVersion}));
    // Recorded and then tested, rather than testing what check returns: a reader of the accesses
    // below, and the analyzer among them, cannot tell that a call decided whether this returns.
    ctx.check(answered.has_value(), "the daemon answers");
    if (!answered)
    {
        return;
    }

    const cmed::protocol::WelcomeMessage welcoming{answered->message};
    const std::optional<cmed::protocol::Welcome_t> welcome = welcoming.read();

    ctx.check(welcoming.isReadable(), "with a Welcome");
    ctx.check(welcoming.sequence() == Asked, "carrying the sequence that was asked");
    ctx.check(welcome && welcome->abiVersion == cmed::protocol::AbiVersion &&
                  welcome->areaBytes == sizeof(cmed::protocol::SharedArea_t),
              "and the abi and size this build lays out");

    if (!ctx.check(answered->descriptors.size() == 1, "one descriptor rides along"))
    {
        return;
    }

    // The area itself, not merely a mapping of the right size. A write through the received
    // descriptor has to land in the words the fixture is holding.
    auto received = cmed::CmedArea::attach(std::move(answered->descriptors.front()));
    cmed::harness::setResult(cmed::harness::resolveSlot(received.shared(), DomainSlot), Witness);

    ctx.check(cmed::harness::resolveSlot(area.shared(), DomainSlot).getFailureCode() == Witness,
              "and it maps the area the daemon is serving");
    ctx.check(countReaches([&serving]
                           {
                               return serving.loop().readCount(cmed::observe::ControlEvent::Welcomed);
                           },
                           1),
              "the daemon counted one welcome");
}

// The bound is on connections held, not on arrivals: the second peer here is admitted by credentials
// and turned away by the count alone.
void anArrivalPastTheBoundIsTurnedAway(probe::Context& ctx, cmed::harness::ProbeArea& area,
                                       cmed::daemon::DaemonConfig_t config,
                                       cmed::daemon::DomainManager& domains)
{
    ctx.openCase("an arrival past control.max_connections");

    config.control.maxConnections = 1;
    const ServingDaemon serving{config, area.descriptor(), area.shared().getAreaBytes(), domains};

    auto first = posix::SeqpacketSocket::connect(config.socketPath());
    ctx.check(greet(first), "the first requester is welcomed");

    // Connect succeeds whatever the daemon decides: the kernel completes it from the listen backlog,
    // and the refusal is the daemon closing what it accepted.
    auto second = posix::SeqpacketSocket::connect(config.socketPath());
    ctx.check(!greet(second), "and the second is closed rather than answered");
    ctx.check(countReaches([&serving]
                           {
                               return serving.loop().readCount(cmed::observe::ControlEvent::Crowded);
                           },
                           1),
              "which the daemon counted as crowding rather than as a misspeaking peer");
    ctx.check(serving.loop().readCount(cmed::observe::ControlEvent::Misspoken) == 0,
              "and nothing was read from it to misjudge");

    // The pair an operator subtracts. One arrival was taken and none has left, which is the reading
    // that says the bound is met rather than that the daemon is idle.
    ctx.check(serving.loop().readCount(cmed::observe::ControlEvent::Admitted) == 1,
              "one arrival was taken onto the loop");
    ctx.check(serving.loop().readCount(cmed::observe::ControlEvent::Departed) == 0,
              "and none of them has left yet");
}

void aVersionThisBuildDoesNotSpeakIsDropped(probe::Context& ctx, cmed::harness::ProbeArea& area,
                                            const cmed::daemon::DaemonConfig_t& config,
                                            cmed::daemon::DomainManager& domains)
{
    ctx.openCase("a protocol version the daemon does not speak");

    const ServingDaemon serving{config, area.descriptor(), area.shared().getAreaBytes(), domains};
    auto asking = posix::SeqpacketSocket::connect(config.socketPath());

    const std::optional<posix::SeqpacketSocket::Received_t> answered =
        ask(asking, headerOnly(cmed::protocol::Version + 1, cmed::protocol::MessageType::Hello));

    ctx.check(!answered, "the connection is closed rather than answered");
    ctx.check(countReaches([&serving]
                           {
                               return serving.loop().readCount(cmed::observe::ControlEvent::Misspoken);
                           },
                           1),
              "and the daemon counted it");
    ctx.check(serving.loop().readCount(cmed::observe::ControlEvent::Welcomed) == 0, "no descriptor was handed over");
}

void anAbiThisBuildDoesNotServeIsDropped(probe::Context& ctx, cmed::harness::ProbeArea& area,
                                         const cmed::daemon::DaemonConfig_t& config,
                                         cmed::daemon::DomainManager& domains)
{
    ctx.openCase("an abi version the daemon does not serve");

    const ServingDaemon serving{config, area.descriptor(), area.shared().getAreaBytes(), domains};
    auto asking = posix::SeqpacketSocket::connect(config.socketPath());

    const std::optional<posix::SeqpacketSocket::Received_t> answered = ask(
        asking, cmed::protocol::HelloMessage::frame(cmed::protocol::Version, Asked, cmed::protocol::Hello_t{cmed::protocol::AbiVersion + 1}));

    ctx.check(!answered, "the connection is closed before any descriptor crosses");
    ctx.check(serving.loop().readCount(cmed::observe::ControlEvent::Welcomed) == 0, "and nothing was welcomed");
}

// One request, one descriptor. A second Hello answered would hand the same area over twice for one
// admission, which is work a requester can ask for without limit.
void aSecondHelloOnOneConnectionIsDropped(probe::Context& ctx, cmed::harness::ProbeArea& area,
                                          const cmed::daemon::DaemonConfig_t& config,
                                          cmed::daemon::DomainManager& domains)
{
    ctx.openCase("Hello twice on one connection");

    const ServingDaemon serving{config, area.descriptor(), area.shared().getAreaBytes(), domains};
    auto asking = posix::SeqpacketSocket::connect(config.socketPath());

    const std::string hello =
        cmed::protocol::HelloMessage::frame(cmed::protocol::Version, Asked, cmed::protocol::Hello_t{cmed::protocol::AbiVersion});

    if (!ctx.check(ask(asking, hello).has_value(), "the first is answered"))
    {
        return;
    }

    ctx.check(!ask(asking, hello), "and the second closes the connection");

    // Exactly one, not at least one. The receive above has already been through the loop's answer to
    // the second Hello, so a second welcome would be counted by now if there were one.
    ctx.checkf(serving.loop().readCount(cmed::observe::ControlEvent::Welcomed) == 1,
               "one welcome and not two (%" PRIu64 ")",
               serving.loop().readCount(cmed::observe::ControlEvent::Welcomed));
}

// A message type this build does not know is counted and discarded. Ending the connection over one
// would stop a requester that is otherwise speaking the protocol correctly.
void anUnknownTypeKeepsTheConnection(probe::Context& ctx, cmed::harness::ProbeArea& area,
                                     const cmed::daemon::DaemonConfig_t& config,
                                     cmed::daemon::DomainManager& domains)
{
    ctx.openCase("a message type the daemon does not know");

    const ServingDaemon serving{config, area.descriptor(), area.shared().getAreaBytes(), domains};
    auto asking = posix::SeqpacketSocket::connect(config.socketPath());

    // Past every name in the enum on purpose, so it stays unknown as the protocol grows; the analyzer
    // warning below is reporting exactly that.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto unknown = static_cast<cmed::protocol::MessageType>(4096);
    if (!ctx.check(asking.send(headerOnly(cmed::protocol::Version, unknown), {}), "the message is sent"))
    {
        return;
    }
    ctx.check(countReaches([&serving]
                           {
                               return serving.loop().readCount(cmed::observe::ControlEvent::Misspoken);
                           },
                           1),
              "the daemon counts it");

    // The connection is what the case is about: the Hello after it has to be answered on the same one.
    const std::optional<posix::SeqpacketSocket::Received_t> answered =
        ask(asking, cmed::protocol::HelloMessage::frame(cmed::protocol::Version, Asked, cmed::protocol::Hello_t{cmed::protocol::AbiVersion}));

    ctx.check(answered.has_value() && answered->descriptors.size() == 1,
              "and still answers the Hello that follows");
}

// A control request carries a name and gets an errno back. The refusal travels in the Answer rather than
// as a dropped connection, because a peer that got this far speaks the protocol and can be told.
void aControlRequestIsAnswered(probe::Context& ctx, cmed::harness::ProbeArea& area,
                               const cmed::daemon::DaemonConfig_t& config, cmed::daemon::DomainManager& domains)
{
    ctx.openCase("CreateDomain");

    const ServingDaemon serving{config, area.descriptor(), area.shared().getAreaBytes(), domains};
    auto asking = posix::SeqpacketSocket::connect(config.socketPath());

    if (!ctx.check(greet(asking), "the connection is welcomed first"))
    {
        return;
    }

    cmed::protocol::DomainRequest_t wanted{};
    if (const auto named = cmed::protocol::DomainName::make(MadeName))
    {
        named->write(wanted.name);
    }

    const std::optional<posix::SeqpacketSocket::Received_t> answered = ask(
        asking, cmed::protocol::DomainRequestMessage::frame(cmed::protocol::MessageType::CreateDomain, cmed::protocol::Version, Asked + 1, wanted));
    ctx.check(answered.has_value(), "the daemon answers the request");
    if (!answered)
    {
        return;
    }

    const cmed::protocol::AnswerMessage answering{answered->message};
    const std::optional<cmed::protocol::Answer_t> result = answering.read();

    ctx.check(answering.isReadable(), "with an Answer");
    ctx.check(answering.sequence() == Asked + 1, "carrying the sequence that was asked");
    ctx.check(result && result->result == asReplyWord(cmed::daemon::DomainAnswer::Served),
              "and the answer the manager gave the create");

    // The name is what the manager was told to act on, and the table it publishes into is the one
    // every requester resolves against.
    ctx.check(area.shared().resolve(MadeName) != cmed::protocol::NoDomain, "with the name the request carried");

    // The other half of one exchange. A refusal reaches the peer as an errno rather than as a dropped
    // connection, so the same socket carries the request after it.
    const std::optional<std::int32_t> refused =
        askAbout(asking, cmed::protocol::MessageType::DeleteDomain, Asked + 2, AbsentName);
    ctx.checkf(refused == asReplyWord(cmed::daemon::DomainAnswer::NoSuchDomain),
               "a delete of a name no domain carries is refused in the Answer, and it says %d",
               refused.value_or(0));

    ctx.check(countReaches([&serving]
                           {
                               return serving.loop().readCount(cmed::observe::ControlEvent::Answered);
                           },
                           2),
              "the daemon counted both");
}

// The area comes first. A create answered before the Welcome would be answered for a connection that may
// yet be refused one, so the two orders are kept from differing by refusing the early request.
void aControlRequestBeforeTheWelcomeIsDropped(probe::Context& ctx, cmed::harness::ProbeArea& area,
                                              const cmed::daemon::DaemonConfig_t& config,
                                              cmed::daemon::DomainManager& domains)
{
    ctx.openCase("CreateDomain before Hello");

    const ServingDaemon serving{config, area.descriptor(), area.shared().getAreaBytes(), domains};
    auto asking = posix::SeqpacketSocket::connect(config.socketPath());

    ctx.check(!askAbout(asking, cmed::protocol::MessageType::CreateDomain, Asked, EarlyName),
              "the connection is closed rather than answered");

    // Nothing stands under the name, which is what says the request stopped at the loop: a create the
    // manager had served would have published it.
    ctx.check(area.shared().resolve(EarlyName) == cmed::protocol::NoDomain, "and the manager never saw the name");
}

// The only signal a requester that died leaves. Unread, this node stays inside a domain no other node
// may then delete, so what the departure reaches the manager with is the whole of the recovery.
void aConnectionThatDropsGivesBackWhatItHeld(probe::Context& ctx, cmed::harness::ProbeArea& area,
                                             const cmed::daemon::DaemonConfig_t& config,
                                             cmed::daemon::DomainManager& domains)
{
    ctx.openCase("a connection that drops without leaving");

    const ServingDaemon serving{config, area.descriptor(), area.shared().getAreaBytes(), domains};

    {
        auto asking = posix::SeqpacketSocket::connect(config.socketPath());
        if (!ctx.check(greet(asking), "the connection is welcomed first"))
        {
            return;
        }

        ctx.check(askAbout(asking, cmed::protocol::MessageType::CreateDomain, Asked + 1, HeldFirstName) ==
                      asReplyWord(cmed::daemon::DomainAnswer::Served),
                  "a create is served");
        ctx.check(askAbout(asking, cmed::protocol::MessageType::CreateDomain, Asked + 2, HeldSecondName) ==
                      asReplyWord(cmed::daemon::DomainAnswer::Served),
                  "and so is a create of a second domain");
    }

    // The scope closed the socket without a leave, which is what a requester that died leaves behind.
    // One epoll for every descriptor, so the pass that admits the socket below handles that hangup too.
    auto following = posix::SeqpacketSocket::connect(config.socketPath());
    if (!ctx.check(greet(following), "a connection after it is welcomed"))
    {
        return;
    }

    // A delete is the exit for the last participant, so a node outside the domain is refused one. Being
    // refused is what says the departure gave this name back, since nothing else here records a leave.
    ctx.check(askAbout(following, cmed::protocol::MessageType::DeleteDomain, Asked + 3, HeldFirstName) ==
                  asReplyWord(cmed::daemon::DomainAnswer::NotParticipating),
              "the departure gave back the first name the connection held");
    ctx.check(askAbout(following, cmed::protocol::MessageType::DeleteDomain, Asked + 4, HeldSecondName) ==
                  asReplyWord(cmed::daemon::DomainAnswer::NotParticipating),
              "and the second, so it reached one leave per name");
}

// A daemon that answers one connection with bytes the case chose. Its own listener rather than a
// ControlHandler, because what these cases need is an answer no correct daemon would send.
//
// The requester's half of the handshake has the same shape as the daemon's: it refuses before it maps
// anything, since a mapping it will reject costs an address space it then has to trust.
class MisleadingDaemon
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // @reply empty closes the connection without answering, which is the one refusal the requester
    // reads as a daemon that is not ready rather than as one it cannot understand.
    MisleadingDaemon(const std::string& path, std::string reply, posix::FileDesc areaDescriptor)
        : listening_{posix::SeqpacketSocket::listen(path, SocketMode, 4)},
          reply_{std::move(reply)},
          areaDescriptor_{areaDescriptor},
          serving_{[this]
                   {
                       serve();
                   }}
    {
    }

    MisleadingDaemon(const MisleadingDaemon&) = delete;
    MisleadingDaemon(MisleadingDaemon&&) = delete;

    ~MisleadingDaemon()
    {
        running_.store(false, std::memory_order_release);
        serving_.join();
    }

    // ── operator= ──────────────────────────────────────────────────────
    MisleadingDaemon& operator=(const MisleadingDaemon&) = delete;
    MisleadingDaemon& operator=(MisleadingDaemon&&) = delete;

private:
    void serve()
    {
        while (running_.load(std::memory_order_acquire))
        {
            posix::UniqueFd taken = listening_.accept();
            if (!taken)
            {
                std::this_thread::sleep_for(cmed::harness::ProbePoll);
                continue;
            }

            auto asking = posix::SeqpacketSocket::adopt(std::move(taken));
            static_cast<void>(asking.receive(cmed::protocol::LongestMessage, cmed::protocol::MostDescriptors,
                                             AnswerWithin));
            if (!reply_.empty())
            {
                std::vector<posix::FileDesc> carried;
                if (areaDescriptor_ >= 0)
                {
                    carried.push_back(areaDescriptor_);
                }
                static_cast<void>(asking.send(reply_, carried));
            }
            return;
        }
    }

private:
    posix::SeqpacketSocket listening_;
    std::string reply_;
    posix::FileDesc areaDescriptor_;
    std::atomic<bool> running_{true};
    std::thread serving_;
};

// Which exception a wrong answer produces is the whole of what a caller acts on: one says wait longer
// and the other says this deployment is wrong. Both are checked, not merely that something threw.
enum class Refusal
{
    Invalid,
    NotReady,
};

[[nodiscard]] bool refusesTheAnswer(const std::string& path, const std::string& reply,
                                    posix::FileDesc areaDescriptor, Refusal wanted)
{
    const MisleadingDaemon serving{path, reply, areaDescriptor};

    try
    {
        static_cast<void>(cmed::CmedSession::connect(path));
    }
    catch (const cmed::CmedAreaInvalidError&)
    {
        return wanted == Refusal::Invalid;
    }
    catch (const cmed::CmedAreaNotReadyError&)
    {
        return wanted == Refusal::NotReady;
    }
    return false;
}

// The requester's own refusals, which no daemon in this tree would ever produce. Each is what stands
// between a mismatched area and a mapping this build then reads with the wrong layout on it.
void aWelcomeTheRequesterWillNotTake(probe::Context& ctx, cmed::harness::ProbeArea& area,
                                     const cmed::harness::ProbeScratch& scratch)
{
    ctx.openCase("a Welcome the requester refuses");

    // Its own path, since each sub-case binds one and takes it away again with its listener.
    const std::string path = scratch.makePath("misleading.sock");
    const auto areaBytes = static_cast<std::uint32_t>(sizeof(cmed::protocol::SharedArea_t));

    // The client stamps its Hello with the sequence its counter starts at, and the answer echoes it.
    // One that does not is an answer to a request this session abandoned.
    ctx.check(refusesTheAnswer(path,
                               cmed::protocol::WelcomeMessage::frame(cmed::protocol::Version, 999,
                                                                     cmed::protocol::Welcome_t{cmed::protocol::AbiVersion, areaBytes}),
                               area.descriptor(), Refusal::Invalid),
              "a Welcome echoing another sequence is refused");

    ctx.check(refusesTheAnswer(path,
                               cmed::protocol::WelcomeMessage::frame(cmed::protocol::Version, 1,
                                                                     cmed::protocol::Welcome_t{cmed::protocol::AbiVersion + 1, areaBytes}),
                               area.descriptor(), Refusal::Invalid),
              "and so is one carrying an abi this build does not lay out");

    ctx.check(refusesTheAnswer(path,
                               cmed::protocol::WelcomeMessage::frame(cmed::protocol::Version, 1,
                                                                     cmed::protocol::Welcome_t{cmed::protocol::AbiVersion, areaBytes + 8}),
                               area.descriptor(), Refusal::Invalid),
              "and one naming a size this build does not lay the area out to");

    // Refused before the mapping either way, which is why this is a separate answer from the three
    // above: the size and the abi agreed and there was still nothing to map.
    ctx.check(refusesTheAnswer(path,
                               cmed::protocol::WelcomeMessage::frame(cmed::protocol::Version, 1,
                                                                     cmed::protocol::Welcome_t{cmed::protocol::AbiVersion, areaBytes}),
                               -1, Refusal::Invalid),
              "a Welcome carrying no descriptor is refused rather than mapped");

    // An Answer where a Welcome belongs. The type is what isReadable tests, so this never reaches the
    // payload at all, and a requester that read it anyway would take an errno for an area size.
    ctx.check(refusesTheAnswer(path,
                               cmed::protocol::AnswerMessage::frame(cmed::protocol::Version, 1,
                                                                    cmed::protocol::Answer_t{0}),
                               area.descriptor(), Refusal::Invalid),
              "and so is an answer of another type entirely");

    // The one refusal that is not the deployment being wrong. A daemon that closed may be one still
    // starting, so the caller is told to wait rather than told to stop.
    ctx.check(refusesTheAnswer(path, "", -1, Refusal::NotReady),
              "a daemon that closes without answering reads as one that is not ready yet");
}

// The two halves in one process, each running its own code. Every other case here builds a message by
// hand, so this is the only one that would notice the client and the daemon disagreeing.
void aRequesterLocksThroughTheRealHandshake(probe::Context& ctx, cmed::harness::ProbeArea& area,
                                            const cmed::daemon::DaemonConfig_t& config,
                                            cmed::daemon::DomainManager& domains)
{
    ctx.openCase("a session built by connecting");

    const ServingDaemon serving{config, area.descriptor(), area.shared().getAreaBytes(), domains};

    // The grant comes from the stub and not from the manager: no serve loop turns here, so a request
    // ringing the doorbell would otherwise wait out the requester's whole deadline.
    const cmed::harness::StubDaemon granting{area.shared()};

    auto session = cmed::CmedSession::connect(config.socketPath());
    {
        const cmed::CmedGuard guard = session.lock(DomainName);
        ctx.check(static_cast<bool>(guard) && cmed::harness::isHeld(area.shared(), DomainSlot),
                  "the domain it locks is the one the daemon handed over");
    }

    ctx.check(cmed::harness::isIdle(area.shared(), DomainSlot), "and the release returns it to Idle");
    static_cast<void>(granting.awaitGrants(1));
    ctx.check(granting.grants() == 1, "one grant, through the area rather than the socket");
}

}  // namespace

int main()
{
    cmed::harness::ProbeScratch scratch{"handshake-probe"};
    return probe::run("handshake probe",
                      [&scratch](probe::Context& ctx)
                      {
                          const std::string areaLabel = scratch.makeAreaName("");
                          const std::string regionLabel = scratch.makeAreaName("region");
                          // Relative: sun_path is a fixed field, and the build tree's absolute path is
                          // close enough to that ceiling to fail for a reason unrelated to the case.
                          cmed::daemon::DaemonConfig_t config;
                          config.socket.dir = scratch.readDirectory();
                          config.area.name = "handshake";
                          config.socket.mode = SocketMode;
                          config.control.idleInterval = IdleTurn;
                          cmed::harness::ProbeArea area{areaLabel.c_str()};
                          cmed::harness::ProbeRegion region{regionLabel.c_str(), RegionSlots};

                          // Named on both sides before the manager reads the region, since its first
                          // pass retires a slot carrying a name the region does not have.
                          cmed::harness::publishDomain(area.shared(), DomainSlot, DomainName);
                          region.createDomain(DomainName);

                          cmed::daemon::DomainManager domains{region.session(), area.shared(), config};

                          theSocketPathBothEndsCompose(ctx);
                          aHelloIsAnsweredWithTheArea(ctx, area, config, domains);
                          anArrivalPastTheBoundIsTurnedAway(ctx, area, config, domains);
                          aVersionThisBuildDoesNotSpeakIsDropped(ctx, area, config, domains);
                          anAbiThisBuildDoesNotServeIsDropped(ctx, area, config, domains);
                          aSecondHelloOnOneConnectionIsDropped(ctx, area, config, domains);
                          anUnknownTypeKeepsTheConnection(ctx, area, config, domains);
                          aControlRequestIsAnswered(ctx, area, config, domains);
                          aControlRequestBeforeTheWelcomeIsDropped(ctx, area, config, domains);
                          aConnectionThatDropsGivesBackWhatItHeld(ctx, area, config, domains);
                          aWelcomeTheRequesterWillNotTake(ctx, area, scratch);
                          aRequesterLocksThroughTheRealHandshake(ctx, area, config, domains);
                      });
}
