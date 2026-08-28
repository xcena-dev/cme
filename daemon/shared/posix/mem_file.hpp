// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/posix/mem_file.hpp -- an anonymous in-memory file and the creator's view of it.
//
// No name anywhere, so nothing can be created at that name first and adopted by mistake; the
// descriptor replaces it, and travels over a socket to whoever is meant to have it.
//
// The mapping comes with it because the size is one fact: stated at create and never again. A caller
// that mapped separately would state it twice, and a mapping shorter than the file hides its tail
// while a longer one faults past its end.

#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

#include "shared/posix/mapping.hpp"
#include "shared/posix/unique_fd.hpp"

namespace posix
{

class MemFile
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    MemFile(const MemFile&) = delete;
    MemFile(MemFile&&) noexcept = default;
    ~MemFile() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────
    MemFile& operator=(const MemFile&) = delete;
    MemFile& operator=(MemFile&&) noexcept = default;

    // ── factories ──────────────────────────────────────────────────
    // @name is for /proc and nothing else: it is not a path, and two of these may share one. Sealing is
    // only ever allowed here, because a descriptor created without MFD_ALLOW_SEALING can never seal.
    [[nodiscard]] static MemFile create(const std::string& name, std::uint64_t bytes)
    {
        UniqueFd held{::memfd_create(name.c_str(), MFD_CLOEXEC | MFD_ALLOW_SEALING)};
        if (!held)
        {
            throw std::system_error{errno, std::system_category(), "memfd_create(" + name + ")"};
        }
        if (::ftruncate(held.get(), static_cast<::off_t>(bytes)) != 0)
        {
            throw std::system_error{errno, std::system_category(), "ftruncate(" + name + ")"};
        }

        // Makes the size above unchangeable: a holder that shrinks the file leaves every other mapping
        // faulting past the new end. Writes stay open, and F_SEAL_SEAL blocks a later F_SEAL_FUTURE_WRITE.
        if (::fcntl(held.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0)
        {
            throw std::system_error{errno, std::system_category(), "F_ADD_SEALS(" + name + ")"};
        }

        return adopt(std::move(held));
    }

    // A file someone else made, whose descriptor this takes and maps whole. The length comes from the
    // file rather than the caller, so no mapping can reach past the end and fault where it reads.
    [[nodiscard]] static MemFile adopt(UniqueFd held)
    {
        struct ::stat found = {};
        if (::fstat(held.get(), &found) != 0)
        {
            throw std::system_error{errno, std::system_category(), "fstat(memfd)"};
        }

        auto mapping = SharedMapping::mmap(held.get(), static_cast<std::uint64_t>(found.st_size));
        return MemFile{std::move(held), std::move(mapping)};
    }

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] FileDesc descriptor() const noexcept
    {
        return held_.get();
    }

    [[nodiscard]] void* base() const noexcept
    {
        return mapping_.base();
    }

    [[nodiscard]] std::uint64_t bytes() const noexcept
    {
        return mapping_.bytes();
    }

private:
    MemFile(UniqueFd held, SharedMapping mapping) noexcept
        : held_{std::move(held)},
          mapping_{std::move(mapping)}
    {
    }

private:
    UniqueFd held_;
    SharedMapping mapping_;
};

}  // namespace posix
