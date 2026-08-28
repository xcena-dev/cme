// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// client/session.cpp -- see cmed/session.hpp. Reaches the daemon and takes the area's descriptor from
// its Welcome, resolves names through a cache, runs the control exchanges over that one connection,
// and asks for turns, holding the domain's mutex across the whole acquire as the only thing
// excluding two local requesters.

#include "cmed/session.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cmed/config.hpp"
#include "cmed/errors.hpp"
#include "cmed/guard.hpp"
#include "cmed/robust_lock.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "shared/area.hpp"
#include "shared/posix/seqpacket_socket.hpp"
#include "shared/protocol/domain_name.hpp"
#include "shared/protocol/id_lookup.hpp"
#include "shared/protocol/message.hpp"
#include "shared/protocol/messages.hpp"
#include "shared/protocol/shared_area.hpp"
#include "shared/util/occupancy.hpp"

namespace cmed
{

// The Hello's sequence number. The session's control exchanges count on from it, so an answer to the
// Hello is never mistaken for one to a later request.
constexpr std::uint32_t StartSequence = 1;

// What this session holds onto the daemon by. Named for what it is to a requester rather than for the
// socket kind, which the calls on it already say.
using Connection = posix::SeqpacketSocket;

namespace
{

// Reaching the daemon. Three failures say a later call may find it there: no socket bound yet, one
// bound with nobody accepting yet, and a full backlog. The rest are this deployment wrong about the path.
Connection connectToDaemon(const std::string& path)
{
    try
    {
        return posix::SeqpacketSocket::connect(path);
    }
    catch (const std::system_error& failure)
    {
        const auto& reason = failure.code();
        const bool notYet = reason == std::errc::no_such_file_or_directory ||
                            reason == std::errc::connection_refused ||
                            reason == std::errc::resource_unavailable_try_again;
        if (notYet)
        {
            throw CmedAreaNotReadyError{"cmed: no daemon is listening on " + path};
        }

        throw CmedBackendError{"cmed: cannot reach " + path, reason};
    }
}

// One greeting and the answer to it, ending in the area itself. What goes out is this build's
// protocol::AbiVersion under a sequence the answer has to echo; four things hold before anything is mapped.
// What a refusal of @type is worded with. Beside the four it words, so a fifth command cannot be
// added without one.
[[nodiscard]] const char* nameOfCommand(protocol::MessageType type) noexcept
{
    switch (type)
    {
        case protocol::MessageType::CreateDomain:
            return "create";
        case protocol::MessageType::DeleteDomain:
            return "delete";
        case protocol::MessageType::JoinDomain:
            return "join";
        case protocol::MessageType::LeaveDomain:
            return "leave";
        default:
            // Unreachable: the four above are every type this session sends a DomainRequest under.
            return "carry out";
    }
}

// The frame both domain-request senders build: this build's protocol version, the caller's sequence,
// and no descriptors, which is every field of it that is not the caller's to choose.
[[nodiscard]] bool sendDomainRequest(Connection& connection, protocol::MessageType type, std::uint32_t& sequence,
                                     const protocol::DomainName& name)
{
    protocol::DomainRequest_t wanted;
    name.write(wanted.name);

    // Taken by reference and raised here, so the number this went out under is the one the caller
    // matches the answer against. Its caller holds the mutex that makes the pair one step.
    ++sequence;

    return connection.send(protocol::DomainRequestMessage::frame(type, protocol::Version, sequence, wanted), {});
}

// One answer off the connection, or nothing when the deadline passed or the daemon went. A message
// past what this protocol asks for is a peer not speaking it, which is the same failure at both callers.
[[nodiscard]] std::optional<posix::SeqpacketSocket::Received_t>
receiveAnswer(Connection& connection, timing::Millis within)
{
    try
    {
        return connection.receive(protocol::LongestMessage, protocol::MostDescriptors, within);
    }
    catch (const std::system_error& failure)
    {
        throw CmedAreaInvalidError{std::string{"cmed: the daemon's answer does not fit this protocol: "} +
                                   failure.code().message()};
    }
}

CmedArea askForArea(Connection& connection, timing::Millis timeout)
{
    const auto hello = protocol::HelloMessage::frame(protocol::Version, StartSequence,
                                                     protocol::Hello_t{protocol::AbiVersion});
    if (!connection.send(hello, {}))
    {
        throw CmedBackendError{"cmed: could not greet the daemon", lastSystemError()};
    }

    auto answered = receiveAnswer(connection, timeout);
    // The daemon drops the connection rather than refusing, since a peer that disagrees about the
    // protocol cannot be told so in that protocol.
    if (!answered)
    {
        throw CmedAreaNotReadyError{"cmed: the daemon closed the connection without answering"};
    }

    const protocol::WelcomeMessage welcoming{answered->message};
    if (!welcoming.isReadable() || welcoming.sequence() != StartSequence)
    {
        throw CmedAreaInvalidError{"cmed: the daemon did not answer with a Welcome to this Hello"};
    }

    const auto welcome = welcoming.read();
    if (!welcome || welcome->abiVersion != protocol::AbiVersion ||
        welcome->areaBytes != sizeof(protocol::SharedArea_t))
    {
        throw CmedAreaInvalidError{"cmed: the daemon serves an area this build does not lay out"};
    }

    if (answered->descriptors.size() != 1)
    {
        throw CmedAreaInvalidError{"cmed: the Welcome carried no single area descriptor"};
    }

    // The mapping last, so a disagreement above costs one message instead of an address space the
    // caller then has to trust.
    return CmedArea::attach(std::move(answered->descriptors.front()));
}

// The one place a caller's text becomes a name, so nothing inside asks the question again.
// Throws CmedInvalidArgumentError for a text no registry slot could hold.
[[nodiscard]] protocol::DomainName makeDomainName(std::string_view name)
{
    const auto named = protocol::DomainName::make(name);
    if (!named)
    {
        throw CmedInvalidArgumentError{"cmed: a domain name must fit the registry's field"};
    }

    return *named;
}

}  // namespace

// ── CmedSession ─────────────────────────────────────────────────────────

struct CmedSession::Impl
{
    // A constructor rather than aggregate initialisation: the mutex below leaves this type
    // unmovable, so a caller would otherwise have to build one to move it in.
    Impl(Connection taken, CmedArea mapped, CmedClientConfig_t chosen)
        : connection_{std::move(taken)},
          area_{std::move(mapped)},
          config_{std::move(chosen)}
    {
    }

    // Gives back what this session joined, so a domain it was the last participant of can still be
    // deleted. Answers are not waited for: the connection closes next and has nowhere to put one.
    ~Impl() noexcept
    {
        try
        {
            const std::lock_guard<std::mutex> held{control_.mutex};
            for (const protocol::DomainName& name : control_.joined)
            {
                static_cast<void>(sendDomainRequest(connection_, protocol::MessageType::LeaveDomain,
                                                    control_.sequence, name));
            }
        }
        catch (const std::exception&)
        {
            // @expected: a teardown has nobody to report to, and the daemon notices the connection close either way.
        }
    }

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] protocol::SharedArea_t& shared() noexcept
    {
        return area_.shared();
    }

    [[nodiscard]] const CmedClientConfig_t& getConfig() const noexcept
    {
        return config_;
    }

    // Whether the daemon is still there, asked before a request is published so a requester does not
    // queue for one that will never answer. The kernel reports the hangup without either side writing.
    [[nodiscard]] bool isServing() const noexcept
    {
        return !connection_.isPeerGone();
    }

    // A create leaves this session in the domain, since the region makes the creator a participant.
    void create(const protocol::DomainName& name)
    {
        const std::lock_guard<std::mutex> held{control_.mutex};
        exchangeAcrossMigrationLocked(protocol::MessageType::CreateDomain, name);
        control_.joined.emplace(name);
    }

    // A delete retracts the participation on its way out, so the record goes without a leave behind it.
    void remove(const protocol::DomainName& name)
    {
        const std::lock_guard<std::mutex> held{control_.mutex};
        exchangeAcrossMigrationLocked(protocol::MessageType::DeleteDomain, name);
        control_.joined.erase(name);
    }

    // The set and the message move together, so two threads joining one name send one join between them.
    void join(const protocol::DomainName& name)
    {
        const std::lock_guard<std::mutex> held{control_.mutex};
        if (control_.joined.count(name) != 0)
        {
            return;
        }

        exchangeAcrossMigrationLocked(protocol::MessageType::JoinDomain, name);
        control_.joined.emplace(name);
    }

    // Leaving what this session never joined asks the daemon nothing: that count belongs to
    // whoever raised it.
    void leave(const protocol::DomainName& name)
    {
        const std::lock_guard<std::mutex> held{control_.mutex};
        if (control_.joined.erase(name) == 0)
        {
            return;
        }

        exchangeAcrossMigrationLocked(protocol::MessageType::LeaveDomain, name);
    }

    // Bounded acquire. nullopt on the deadline alone; a name no live domain carries still throws. A
    // daemon replaced under this call is not the caller's business: the next attempt asks the area its
    // replacement serves, inside the same deadline.
    [[nodiscard]] std::optional<CmedGuard> tryLock(const protocol::DomainName& name, timing::Nanos timeout)
    {
        const timing::Deadline deadline{timeout};

        for (;;)
        {
            const auto left = deadline.remaining<timing::Nanos>();
            if (!left)
            {
                return std::nullopt;
            }

            auto guard = tryLockOnce(name, *left);

            // A held turn is the answer, and so is an empty hand from a daemon that is still there. A
            // closed door is neither: the replacement it belongs to has yet to publish its area.
            if (guard || (isServing() && !closing_.load(std::memory_order_seq_cst)))
            {
                return guard;
            }

            // Slept outside the mutex, so a caller waiting out a replacement holds nothing the
            // control operations need.
            if (!migrate(*left))
            {
                // A replacement takes long enough to come up that a tighter step would only spend
                // the wait on retries.
                constexpr timing::Millis MigrateRetryStep{20};
                std::this_thread::sleep_for(MigrateRetryStep);
            }
        }
    }

private:
    // ── one attempt ────────────────────────────────────────────────
    // One attempt on the area being served now. nullopt for the deadline and for a daemon that went,
    // which its caller tells apart.
    [[nodiscard]] std::optional<CmedGuard> tryLockOnce(const protocol::DomainName& name, timing::Nanos timeout)
    {
        // Before the area is read, and given back on every exit below: a replacement unmaps what the
        // reference is to, and this is what makes it wait for this attempt instead.
        const util::OccupancyToken attempting{holders_};

        // Fenced, because this side stores its own word and then reads the replacement's while the
        // replacement does the reverse. A weaker pair lets both sides conclude the area is theirs.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (closing_.load(std::memory_order_seq_cst))
        {
            return std::nullopt;
        }

        auto& area = this->shared();
        const auto domainId = resolveId(name);
        if (domainId == protocol::NoDomain)
        {
            throw CmedUnknownDomainError{"cmed: no live domain named " + std::string{name.getText()}};
        }

        auto& context = area.domain.table[domainId];

        // The mutex first and on its own: whether the daemon has to be asked at all is the next
        // question, and only the mutex holder may ask it.
        auto held = context.lockForRequest();

        // Again under the mutex. The id was read before it, and a slot retired and reclaimed in that gap
        // answers a different name; a retire cannot happen from here on, since it needs this mutex free.
        if (!context.isNamed(name.getText()))
        {
            throw CmedUnknownDomainError{"cmed: no live domain named " + std::string{name.getText()}};
        }

        // The claim goes down before the turn behind it is read: the daemon zeroes validity then
        // reads this state, so reading first here would let both sides conclude the turn is theirs.
        context.publish(protocol::RequestState::LockHeld);
        if (context.hasValidTurn())
        {
            return CmedGuard{area, holders_, domainId, std::move(held)};
        }

        // Nothing stands behind the claim, so it comes back down; no other requester saw it, since
        // this thread holds the mutex.
        context.publish(protocol::RequestState::Idle);

        // Only the slow path pays for this: a requester queued on a daemon that is not taking turns
        // would otherwise wait out its own deadline for nobody.
        if (!isServing())
        {
            return std::nullopt;
        }

        area.request(domainId, protocol::RequestState::LockRequested);

        // Built before the answer is judged, so every exit runs the same revoke-and-Idle sequence.
        CmedGuard guard{area, holders_, domainId, std::move(held)};

        const auto outcome = context.awaitState(timeout, protocol::isAcquireAnswered, timing::Nanos{config_.spin});
        if (!outcome)
        {
            return std::nullopt;
        }
        if (*outcome == protocol::RequestState::Error)
        {
            const auto failure = context.getFailureCode();
            throw CmedLockRefusedError{"cmed: daemon refused " + std::string{name.getText()},
                                       std::error_code{-failure, std::generic_category()}};
        }

        return guard;
    }

    // The id a name resolves to, out of the cache when that slot still carries the name: a slot
    // retired and reclaimed answers a different name, so the check fails and this rescans.
    [[nodiscard]] std::uint32_t resolveId(const protocol::DomainName& name)
    {
        const auto& shared = area_.shared();

        {
            const std::lock_guard<std::mutex> held{lookup_.mutex};
            const auto cached = lookup_.ids.find(name);
            if (cached && shared.domain.table[*cached].isNamed(name.getText()))
            {
                return *cached;
            }
        }

        const auto scanned = shared.resolve(name.getText());
        if (scanned == protocol::NoDomain)
        {
            // Not cached: the daemon publishes names as it finds them, so a cached absence would
            // outlive the reason for it.
            return protocol::NoDomain;
        }

        const std::lock_guard<std::mutex> held{lookup_.mutex};
        lookup_.ids.set(name, scanned);
        return scanned;
    }

    // One exchange across a replacement, with control_.mutex already held. A refusal from a daemon that
    // is still there is its answer and stands; one from a daemon that has gone is asked of the next.
    void exchangeAcrossMigrationLocked(protocol::MessageType type, const protocol::DomainName& name)
    {
        try
        {
            exchangeDomainRequestLocked(type, name);
            return;
        }
        catch (const CmedControlRefusedError&)
        {
            if (isServing())
            {
                throw;
            }
        }

        if (!migrateLocked(timing::Nanos{config_.lockTimeout}))
        {
            throw CmedControlRefusedError{"cmed: a caller still holds a turn on the area the daemon left",
                                          std::error_code{EBUSY, std::system_category()}};
        }

        exchangeDomainRequestLocked(type, name);
    }
    // One exchange, with control_.mutex already held. Apart from its callers so one needing the set and
    // the message to move together can hold the lock across both. A refusal leaves as an exception.
    void exchangeDomainRequestLocked(protocol::MessageType type, const protocol::DomainName& name)
    {
        if (!sendDomainRequest(connection_, type, control_.sequence, name))
        {
            throw CmedControlRefusedError{"cmed: could not reach the daemon", lastSystemError()};
        }

        const auto answered = receiveAnswer(connection_, config_.lockTimeout);
        if (!answered)
        {
            throw CmedControlRefusedError{"cmed: the daemon did not answer",
                                          std::error_code{ETIMEDOUT, std::system_category()}};
        }

        const protocol::AnswerMessage answering{answered->message};
        const auto result = answering.read();
        if (!result || answering.sequence() != control_.sequence)
        {
            throw CmedControlRefusedError{"cmed: the daemon's answer does not match the request",
                                          std::error_code{EPROTO, std::system_category()}};
        }

        if (result->result != 0)
        {
            throw CmedControlRefusedError{"cmed: the daemon would not " + std::string{nameOfCommand(type)} + " " +
                                              std::string{name.getText()},
                                          std::error_code{-result->result, std::system_category()}};
        }
    }

    // ── migrating to a replacement ─────────────────────────────────
    // Takes the mutex the Locked one below asks for, and answers true for a session on a live daemon
    // whether this caller migrated it or found that another already had.
    [[nodiscard]] bool migrate(timing::Nanos within)
    {
        const std::lock_guard<std::mutex> held{control_.mutex};
        if (isServing())
        {
            return true;
        }

        try
        {
            return migrateLocked(within);
        }
        catch (const CmedAreaNotReadyError&)
        {
            return false;
        }
    }

    // A replacement daemon serves its own area, so nothing of the previous one carries over. The
    // mapping, the resolve cache and the sequence all start again from this Welcome.
    [[nodiscard]] bool migrateLocked(timing::Nanos within)
    {
        // First, and the only step that can be refused: the area below is unmapped by the assignment,
        // and a guard still out of it would write to nothing on its way back.
        if (!closeAndDrain(within))
        {
            return false;
        }

        connection_ = connectToDaemon(config_.socketPath);
        area_ = askForArea(connection_, config_.setupTimeout);
        control_.sequence = StartSequence;

        {
            const std::lock_guard<std::mutex> held{lookup_.mutex};
            lookup_.ids = protocol::IdLookup{};
        }

        rejoinLocked();
        reopenArea();
        return true;
    }

    // The names this session asked to keep, asked of the daemon now serving. A name no live domain
    // carries leaves the set rather than failing the migration, since a caller learns when it next names it.
    void rejoinLocked()
    {
        std::vector<protocol::DomainName> refused;
        for (const protocol::DomainName& name : control_.joined)
        {
            try
            {
                exchangeDomainRequestLocked(protocol::MessageType::JoinDomain, name);
            }
            catch (const CmedControlRefusedError&)
            {
                refused.push_back(name);
            }
        }

        for (const protocol::DomainName& name : refused)
        {
            control_.joined.erase(name);
        }
    }

    // ── the area's holders ─────────────────────────────────────────
    // Stops letting attempts in and waits for the ones already inside. Bounded because a caller
    // holding a guard that asks for a second domain would otherwise wait for itself.
    [[nodiscard]] bool closeAndDrain(timing::Nanos within)
    {
        closing_.store(true, std::memory_order_seq_cst);

        // A critical section is the application's own length, so the count is polled rather than
        // spun on.
        constexpr timing::Micros DrainStep{200};
        const bool drained = poll::waitUntil(
            [this]()
            {
                return holders_.load(std::memory_order_seq_cst) == 0;
            },
            within, timing::Nanos{DrainStep});
        if (!drained)
        {
            // Nothing was replaced, so the door opens again rather than refusing every caller for good.
            closing_.store(false, std::memory_order_seq_cst);
        }

        return drained;
    }

    // Lets attempts in again, on the area the caller replaced while this was closed.
    void reopenArea() noexcept
    {
        closing_.store(false, std::memory_order_seq_cst);
    }

private:
    // This requester's liveness: the daemon's epoll sees it drop, whether the process exited tidily
    // or died.
    Connection connection_;

    CmedArea area_;
    CmedClientConfig_t config_;

    // Everything using that area: one for each attempt in flight and one for each guard out. A
    // replacement waits for this to empty, since the area it unmaps is what all of them name.
    std::atomic<std::uint32_t> holders_{0};

    // Whether a replacement has stopped letting attempts in. Read once before counting in and once
    // after, so a set between the two is seen by the attempt that counted in under it.
    std::atomic<bool> closing_{false};

    // ── the resolve cache ──────────────────────────────────────────────
    // Without it every lock() scans the whole registry, on the path to a domain locked many times already.
    struct
    {
        std::mutex mutex;
        protocol::IdLookup ids;
    } lookup_;

    // ── control ────────────────────────────────────────────────────────
    // One connection, so one request at a time: two threads asking together would each read
    // whichever answer arrived first. joined is what this session asked to keep rather than what the
    // daemon holds, and it is what the destructor gives back on a tidy exit.
    struct
    {
        std::mutex mutex;
        std::uint32_t sequence{StartSequence};
        std::unordered_set<protocol::DomainName> joined;
    } control_;
};

CmedSession::CmedSession(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)}
{
}

CmedSession::CmedSession(CmedSession&&) noexcept = default;
CmedSession& CmedSession::operator=(CmedSession&&) noexcept = default;

CmedSession::~CmedSession() = default;

CmedSession CmedSession::connect(const CmedClientConfig_t& config)
{
    if (config.socketPath.empty())
    {
        throw CmedInvalidArgumentError{"cmed: the config names no daemon socket"};
    }

    Connection connection = connectToDaemon(config.socketPath);
    auto area = askForArea(connection, config.setupTimeout);

    return CmedSession{std::make_unique<Impl>(std::move(connection), std::move(area), config)};
}

CmedSession CmedSession::connect(std::string_view socketPath)
{
    CmedClientConfig_t config;
    config.socketPath = std::string{socketPath};
    return connect(config);
}

// ── domains ─────────────────────────────────────────────────────────────

void CmedSession::createDomain(std::string_view domainName)
{
    impl_->create(makeDomainName(domainName));
}

// Only a domain this node is in: the delete takes the domain's turn, and a peer outside it has no
// turn to take.
void CmedSession::deleteDomain(std::string_view domainName)
{
    impl_->remove(makeDomainName(domainName));
}

// Idempotent per session: what a caller means by asking twice is that it wants the domain, not
// that it wants two of it.
void CmedSession::joinDomain(std::string_view domainName)
{
    impl_->join(makeDomainName(domainName));
}

// A name too wide for the registry names nothing this session joined, so leaving it is the no-op the
// caller asked for rather than the error the other three answer with.
void CmedSession::leaveDomain(std::string_view domainName)
{
    if (const auto named = protocol::DomainName::make(domainName))
    {
        impl_->leave(*named);
    }
}

std::optional<CmedGuard> CmedSession::tryLock(std::string_view domainName, timing::Nanos timeout)
{
    const auto named = protocol::DomainName::make(domainName);
    if (!named)
    {
        throw CmedUnknownDomainError{"cmed: no live domain named " + std::string{domainName}};
    }

    return impl_->tryLock(*named, timeout);
}

CmedGuard CmedSession::lock(std::string_view domainName)
{
    auto guard = tryLock(domainName, impl_->getConfig().lockTimeout);
    if (!guard)
    {
        throw CmedLockTimeoutError{"cmed: timed out waiting for " + std::string{domainName}};
    }
    return std::move(*guard);
}

}  // namespace cmed
