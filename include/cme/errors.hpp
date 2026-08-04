// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// errors.hpp -- cme exception hierarchy. All errors derive from std::runtime_error.

#pragma once

#include <stdexcept>

namespace cme
{

class Error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// me_format / mmap / shm_open setup failed.
class FormatError : public Error
{
public:
    using Error::Error;
};

// Region opened OK but header magic / version mismatch.
class RegionInvalidError : public Error
{
public:
    using Error::Error;
};

// me_join failed (slot busy, already member, etc.).
class JoinError : public Error
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

// open() timed out waiting for format to complete.
class RegionNotFormattedError : public Error
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

// Caller passed a domain name not present in the region.
class UnknownDomainError : public Error
{
public:
    using Error::Error;
};

// lock()/leaveDomain() on a domain not joined by this peer.
class NotParticipatingError : public Error
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

}  // namespace cme
