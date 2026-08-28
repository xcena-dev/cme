// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// daemon/control/exchange.hpp -- answering one message. The header half is the same for every message:
// whose it is, which version to answer in, which sequence to echo. Below it are the two this daemon
// answers, a Hello and the four domain requests, each seeing only its own payload.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

#include "daemon/domain/manager.hpp"
#include "shared/protocol/domain_name.hpp"
#include "shared/protocol/message.hpp"
#include "shared/protocol/messages.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed::daemon
{

// ── what one connection is holding ─────────────────────────────────
// What one connection joined and has not left. The manager's count is a total across the node, so a
// peer that died without leaving is only findable by the names its own connection recorded.
using JoinLedger = std::unordered_set<std::string>;

// ── the half no message does differently ───────────────────────────

class Exchange
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    Exchange() noexcept = default;
    Exchange(const Exchange&) = delete;
    Exchange(Exchange&&) = delete;
    virtual ~Exchange() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────────
    Exchange& operator=(const Exchange&) = delete;
    Exchange& operator=(Exchange&&) = delete;

    // ── public methods ─────────────────────────────────────────────────
    // The bytes to send back, or nothing when this exchange will not answer. Nothing is the caller's
    // cue to drop the connection, since a peer told nothing in a protocol it got wrong learns nothing.
    // @joins is the asking connection's own, since an exchange that changes what a peer holds is the
    // only place that knows both the name and whether the answer took.
    [[nodiscard]] std::optional<std::string> answer(const protocol::Message& arrived, const std::string& message,
                                                    JoinLedger& joins) const
    {
        if (!answers(arrived.type()))
        {
            return std::nullopt;
        }

        return reply(arrived, message, joins);
    }

    // ── accessors ──────────────────────────────────────────────────────
    // Asked before answer(), so a caller holding several exchanges can pick one.
    [[nodiscard]] virtual bool answers(protocol::MessageType type) const noexcept = 0;

protected:
    // ── the payload half ───────────────────────────────────────────────
    // @header is the one already read, so a reply reaches for the version and the sequence rather than
    // parsing the message twice. The version is the peer's: this build's own would be unreadable by it.
    [[nodiscard]] virtual std::optional<std::string> reply(const protocol::Message& arrived,
                                                           const std::string& message,
                                                           JoinLedger& joins) const = 0;
};

// ── one Hello ──────────────────────────────────────────────────────
// The requester's side is its own, in client/. What makes a Hello acceptable is a property of this
// build rather than of the message, so a type both sides compiled would carry it for one of them.

class HelloExchange : public Exchange
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // Only the size: the descriptor a Welcome comes with crosses as ancillary data, so it belongs to
    // whoever calls sendmsg and never to the payload.
    explicit HelloExchange(std::uint32_t areaBytes) noexcept
        : areaBytes_{areaBytes}
    {
    }

    // ── accessors ──────────────────────────────────────────────────────
    [[nodiscard]] bool answers(protocol::MessageType type) const noexcept override
    {
        return type == protocol::MessageType::Hello;
    }

protected:
    // ── the payload half ───────────────────────────────────────────────
    // Refused before the descriptor moves: refusing costs one message, while handing over a mapping
    // the peer then rejects costs the descriptor for nothing.
    [[nodiscard]] std::optional<std::string> reply(const protocol::Message& arrived, const std::string& message,
                                                   JoinLedger& /*joins*/) const override
    {
        const std::optional<protocol::Hello_t> asked = protocol::HelloMessage{message}.read();
        if (!asked || asked->abiVersion != protocol::AbiVersion)
        {
            return std::nullopt;
        }

        return protocol::WelcomeMessage::frame(arrived.version(), arrived.sequence(),
                                               protocol::Welcome_t{protocol::AbiVersion, areaBytes_});
    }

private:
    std::uint32_t areaBytes_;
};

// ── create, delete, join and leave ─────────────────────────────────
// One exchange for the four: they carry one payload and differ only in what the manager does with
// the name. A refusal travels as an errno in the Answer, which is a protocol the peer speaks.

class DomainRequestExchange : public Exchange
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    // @domains has to outlive this, which every run gives it: both are held for the whole of one.
    explicit DomainRequestExchange(DomainManager& domains) noexcept
        : domains_{&domains}
    {
    }

    // ── accessors ──────────────────────────────────────────────────────
    [[nodiscard]] bool answers(protocol::MessageType type) const noexcept override
    {
        return protocol::isDomainRequest(type);
    }

    // ── public methods ─────────────────────────────────────────────────
    // Leaves everything @joins still holds, for a peer that died without leaving any of it. Answers
    // nothing: the connection this ran for is already gone.
    void giveBack(const JoinLedger& joins) const
    {
        for (const std::string& name : joins)
        {
            static_cast<void>(domains_->serveCommand(protocol::MessageType::LeaveDomain, name));
        }
    }

protected:
    // ── the payload half ───────────────────────────────────────────────
    // The type reaches the manager, since that is the whole of what tells the four apart.
    [[nodiscard]] std::optional<std::string> reply(const protocol::Message& arrived, const std::string& message,
                                                   JoinLedger& joins) const override
    {
        const std::optional<protocol::DomainRequest_t> asked =
            protocol::DomainRequestMessage{message, arrived.type()}.read();
        if (!asked)
        {
            return std::nullopt;
        }

        const auto result = static_cast<std::int32_t>(domains_->serveCommand(arrived.type(), protocol::DomainName::read(asked->name)));
        if (result == 0)
        {
            record(arrived.type(), asked->name, joins);
        }

        return protocol::AnswerMessage::frame(arrived.version(), arrived.sequence(),
                                              protocol::Answer_t{result});
    }

private:
    // Only what the answer says took. A create leaves the peer in the domain and a delete takes it
    // out, so the four sort into two: what this connection now holds, and what it no longer does.
    static void record(protocol::MessageType asked, const char (&name)[MaxName], JoinLedger& joins)
    {
        const std::string held{protocol::DomainName::read(name)};

        switch (asked)
        {
            case protocol::MessageType::CreateDomain:
            case protocol::MessageType::JoinDomain:
                static_cast<void>(joins.insert(held));
                break;
            default:
                static_cast<void>(joins.erase(held));
                break;
        }
    }

    // The one manager on this node. Held as a pointer, so answering stays const while what it answers
    // out of does not have to be.
    DomainManager* domains_;
};

}  // namespace cmed::daemon
