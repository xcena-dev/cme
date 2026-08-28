// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cmed/errors.hpp -- cmed exception hierarchy. All errors derive from std::runtime_error.
//
// Separate from cme's hierarchy on purpose. A requester talks to the daemon and never opens a cme
// region, so it must not have to link libcme to catch what its own calls throw.

#pragma once

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>

namespace cmed
{

class CmedError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// ── a failure as a std::error_code ─────────────────────────────────

// Both live here rather than beside their callers: they are the only two ways a failure becomes a
// std::error_code in cmed, and a copy per source file would drift.
[[nodiscard]] inline std::error_code lastSystemError() noexcept
{
    return std::error_code{errno, std::system_category()};
}

// pthread functions return the error instead of setting errno, so their code comes from the return
// value and belongs to the generic category rather than the system one.
[[nodiscard]] inline std::error_code threadError(std::int32_t result) noexcept
{
    return std::error_code{result, std::generic_category()};
}

// A failure that names a std::error_code as well as a message. Its own class because the code goes
// into what() too: a caller reading only what() would otherwise get the message without the reason.
class CmedCodedError : public CmedError
{
public:
    explicit CmedCodedError(const std::string& message, std::error_code code = {})
        : CmedError{code ? message + ": " + code.message() : message},
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

// The shm object could not be opened, sized or mapped. code() carries the errno the failing call
// left, so a caller compares against std::errc instead of reading what().
class CmedBackendError : public CmedCodedError
{
public:
    using CmedCodedError::CmedCodedError;
};

// The caller's input cannot name an area: an empty name, a name past NAME_MAX, a name carrying a
// separator. Nothing about the machine would make a retry succeed.
class CmedInvalidArgumentError : public CmedError
{
public:
    using CmedError::CmedError;
};

// The area mapped, but its abiVersion is one this build does not know. Reading further would
// misread the layout rather than fail.
class CmedAreaInvalidError : public CmedError
{
public:
    using CmedError::CmedError;
};

// No daemon is answering yet: nothing listening on the socket, a connection that accepted and sent
// no Welcome, or a descriptor whose area is unstamped. A later call may find one, which is what
// separates this from CmedBackendError.
class CmedAreaNotReadyError : public CmedError
{
public:
    using CmedError::CmedError;
};

// ── locking ────────────────────────────────────────────────────────

// The name is not a live domain in the registry. Thrown rather than answered with a deadline, so a
// retry loop on a misspelt name fails at once instead of spinning.
class CmedUnknownDomainError : public CmedError
{
public:
    using CmedError::CmedError;
};

// The wait for the daemon's answer hit its deadline. The request may still be in flight, which is
// why the domain mutex is only released after the state machine is back at Idle.
class CmedLockTimeoutError : public CmedError
{
public:
    using CmedError::CmedError;
};

// A create/delete the daemon refused; code() carries the errno. EEXIST/ENOENT are ordinary; EBUSY
// means a local requester still holds it, ENOTCONN means no daemon, EPERM/ENOTEMPTY are delete-only.
class CmedControlRefusedError : public CmedCodedError
{
public:
    using CmedCodedError::CmedCodedError;
};

// The daemon answered Error. code() carries the errno it left in the context's result field.
class CmedLockRefusedError : public CmedCodedError
{
public:
    using CmedCodedError::CmedCodedError;
};

}  // namespace cmed
