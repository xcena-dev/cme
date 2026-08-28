// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/account.hpp -- a user or group named as text, resolved to the id the kernel checks.
//
// A config file names people, and every check downstream is on an id. Both forms are accepted here:
// a number is taken as the id it fits, and a name goes to the password or group database.

#pragma once

#include <grp.h>
#include <pwd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <system_error>

#include "cmed/errors.hpp"

namespace cmed::posix
{

// Empty is not numeric, so a blank entry is refused by name resolution rather than turned into id
// zero, which is root.
[[nodiscard]] inline bool isNumeric(const std::string& written)
{
    const auto digit = [](unsigned char character)
    {
        return std::isdigit(character) != 0;
    };
    return !written.empty() && std::all_of(written.begin(), written.end(), digit);
}

// from_chars parses into the id type itself, so a digit string past that width is refused rather
// than narrowed: a cast would turn 4294967296 into 0, which is root.
template <typename T_Id>
[[nodiscard]] inline T_Id readNumericId(const std::string& written, const std::string& named)
{
    T_Id parsed = 0;
    const char* const past = written.data() + written.size();
    const std::from_chars_result result = std::from_chars(written.data(), past, parsed);
    if (result.ec != std::errc{} || result.ptr != past)
    {
        throw CmedInvalidArgumentError{named + " names no id this host can use: " + written};
    }
    return parsed;
}

// getpwnam rather than its _r form: a config is read once, before the process has a second thread.
// @named is what a refusal quotes, so the reader is told which key held the account it could not find.
[[nodiscard]] inline ::uid_t getUid(const std::string& written, const std::string& named)
{
    if (isNumeric(written))
    {
        return readNumericId<::uid_t>(written, named);
    }

    const ::passwd* const found = ::getpwnam(written.c_str());
    if (found == nullptr)
    {
        throw CmedInvalidArgumentError{named + " names no account on this host: " + written};
    }
    return found->pw_uid;
}

[[nodiscard]] inline ::gid_t getGid(const std::string& written, const std::string& named)
{
    if (isNumeric(written))
    {
        return readNumericId<::gid_t>(written, named);
    }

    const ::group* const found = ::getgrnam(written.c_str());
    if (found == nullptr)
    {
        throw CmedInvalidArgumentError{named + " names no group on this host: " + written};
    }
    return found->gr_gid;
}

}  // namespace cmed::posix
