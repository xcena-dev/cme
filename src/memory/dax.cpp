// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// dax.cpp -- /dev/dax* backend. mmap length is PMD-aligned (2 MiB).

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#include "cme/errors.hpp"
#include "memory/memory.hpp"
#include "util/util.hpp"

namespace cme
{

namespace
{

[[nodiscard]] void* openMmap(std::string_view path, std::uint64_t mapSize, std::uint64_t offset)
{
    const std::string pathString{path};
    // devdax refuses an unaligned mmap offset outright; reject it here so the failure
    // names the offset rather than surfacing as a bare EINVAL.
    if (offset % PmdAlign != 0)
    {
        throw InvalidArgumentError{std::string{"cme::DaxMemory("} + pathString +
                                   "): offset must be a multiple of 2 MiB"};
    }
    const int file = ::open(pathString.c_str(), O_RDWR);
    if (file < 0)
    {
        const auto failure = lastSystemError();
        throw BackendError{std::string{"cme::DaxMemory open("} + pathString + ")", failure};
    }
    void* mapped = ::mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, file,
                          static_cast<::off_t>(offset));
    // Before the close, which is allowed to leave its own value in errno.
    const auto failure = (mapped == MAP_FAILED) ? lastSystemError() : std::error_code{};
    ::close(file);
    if (mapped == MAP_FAILED)
    {
        throw BackendError{std::string{"cme::DaxMemory mmap("} + pathString + ")", failure};
    }
    return mapped;
}

}  // namespace

DaxMemory::DaxMemory(std::string_view path, std::uint64_t offset)
    : Memory{openMmap(path, PmdAlign, offset), PmdAlign}
{
}

DaxMemory::DaxMemory(std::string_view path, std::uint64_t areaSize, std::uint64_t offset)
    : Memory{nullptr, 0}
{
    const std::uint64_t mapSize = roundUp(areaSize, PmdAlign);
    base_ = openMmap(path, mapSize, offset);
    mappedSize_ = mapSize;
}

}  // namespace cme
