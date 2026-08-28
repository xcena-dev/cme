// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cmed/session.hpp -- what a local participant calls to take a domain's turn. One CmedSession per
// process shares the daemon's single peer slot. Past setup, the connection exists only for death
// detection through epoll. THREADING: it excludes this process's own threads on the domain mutex.
//
// What one acquire hands back is a CmedGuard, whose own header is beside this one.

#pragma once

#include <memory>
#include <optional>
#include <string_view>

#include "cmed/config.hpp"
#include "cmed/guard.hpp"
#include "common/timing.hpp"

namespace cmed
{

class CmedSession
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    CmedSession(const CmedSession&) = delete;
    CmedSession(CmedSession&&) noexcept;
    ~CmedSession();  // gives back what it joined, then unmaps the area

    // ── operator= ──────────────────────────────────────────────────
    CmedSession& operator=(const CmedSession&) = delete;
    // Every constructor is private, so re-seating a session a caller already holds goes through
    // here. A daemon replaced under a live caller is what makes that a real path.
    CmedSession& operator=(CmedSession&&) noexcept;

    // ── factories ──────────────────────────────────────────────────
    // Connects to the daemon the config names, in one attempt. Throws CmedAreaNotReadyError where a
    // later call may still find one, CmedAreaInvalidError on an area this build cannot read.
    [[nodiscard]] static CmedSession connect(const CmedClientConfig_t& config);

    // Same, for a caller with only the socket path. Every deadline is the compiled-in default.
    [[nodiscard]] static CmedSession connect(std::string_view socketPath);

    // ── domains ────────────────────────────────────────────────────
    // Creates or deletes @domainName. A delete takes the domain's turn, so a caller deleting a domain
    // it never joined joins it first. Throws CmedInvalidArgumentError or CmedControlRefusedError.
    void createDomain(std::string_view domainName);
    void deleteDomain(std::string_view domainName);

    // Keeps this node participating in @domainName until leaveDomain, and counted, so one leave does
    // not end another's. join throws as createDomain does; a leave of what it never joined is a no-op.
    void joinDomain(std::string_view domainName);
    void leaveDomain(std::string_view domainName);

    // ── ops ────────────────────────────────────────────────────────
    // Blocking acquire. Throws CmedUnknownDomainError, CmedLockTimeoutError, CmedLockRefusedError.
    [[nodiscard]] CmedGuard lock(std::string_view domainName);

    // Bounded acquire. nullopt on deadline only; an unknown name still throws.
    [[nodiscard]] std::optional<CmedGuard> tryLock(std::string_view domainName,
                                                   timing::Nanos timeout);

private:
    struct Impl;
    explicit CmedSession(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace cmed
