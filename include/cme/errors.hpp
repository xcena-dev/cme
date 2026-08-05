// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// errors.hpp -- cme exception hierarchy. All errors derive from std::runtime_error.
//
// Grouped by when the error can reach a caller: opening a region, taking a peer slot, the domain
// registry, locking a domain. Every type derives from Error, and the region-opening group has a
// second level under FormatError that says whether the caller's input or the machine was at fault.

#pragma once

#include <stdexcept>
#include <string>
#include <system_error>

namespace cme
{

class Error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// ── opening a region ───────────────────────────────────────────────

// me_format / mmap / shm_open setup failed. The two below narrow it by what the caller can do
// about it, and both derive from it, so catching FormatError still catches everything.
class FormatError : public Error
{
public:
    using Error::Error;
};

// The URI or the size the caller passed cannot describe a region: bad grammar, an unknown scheme,
// a dax offset off its PMD boundary, an shm name that is empty or past NAME_MAX, a creator asking
// for zero bytes. Nothing about the machine would make a retry succeed.
class InvalidArgumentError : public FormatError
{
public:
    using FormatError::FormatError;
};

// The backing object could not be opened, sized or mapped. code() carries the errno the failing
// call left, so a caller compares against std::errc instead of reading what(). It is empty for the
// refusals with no failing syscall behind them, such as an object that exists at size zero.
class BackendError : public FormatError
{
public:
    explicit BackendError(const std::string& message, std::error_code code = {})
        : FormatError{code ? message + ": " + code.message() : message},
          code_{code}
    {
    }

    [[nodiscard]] const std::error_code& code() const noexcept
    {
        return code_;
    }

private:
    std::error_code code_;
};

// Region opened OK but header magic / version mismatch.
class RegionInvalidError : public Error
{
public:
    using Error::Error;
};

// open() timed out waiting for format to complete.
class RegionNotFormattedError : public Error
{
public:
    using Error::Error;
};

// ── taking a peer slot ─────────────────────────────────────────────

// me_join failed (slot busy, already member, etc.).
class JoinError : public Error
{
public:
    using Error::Error;
};

// Admission claim could not secure a peer slot (lease deadline or table full).
class NoFreeSlotError : public Error
{
public:
    using Error::Error;
};

// ── the domain registry ────────────────────────────────────────────

// Caller passed a domain name not present in the region.
class UnknownDomainError : public Error
{
public:
    using Error::Error;
};

// createDomain() with a name already held by an Active domain.
class DomainExistsError : public Error
{
public:
    using Error::Error;
};

// createDomain() when every data slot is in use (slot ceiling reached).
class DomainLimitError : public Error
{
public:
    using Error::Error;
};

// ── locking a domain ───────────────────────────────────────────────

// lock()/leaveDomain() on a domain not joined by this peer.
class NotParticipatingError : public Error
{
public:
    using Error::Error;
};

// me_lock hit the global 5 s deadline (counter probe path triggered).
class LockTimeoutError : public Error
{
public:
    LockTimeoutError()
        : Error{"cme: lock timed out"}
    {
    }
};

}  // namespace cme
