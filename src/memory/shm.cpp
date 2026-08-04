// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shm.cpp -- POSIX shm_open backend. Dtor never auto-unlinks; the caller
// shm_unlinks when the name should disappear.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "cme/errors.hpp"
#include "memory/memory.hpp"

namespace cme
{

namespace
{

// shm name (including leading slash) must fit in one filesystem component (NAME_MAX=255).
inline constexpr std::size_t MaxShmNameLen = 255;

[[nodiscard]] std::string normaliseShmName(std::string_view name)
{
    if (name.empty())
    {
        throw FormatError{"cme::ShmMemory: empty shm name"};
    }
    std::string normalised;
    if (name.front() == '/')
    {
        normalised.assign(name.data(), name.size());
    }
    else
    {
        normalised.reserve(name.size() + 1);
        normalised.push_back('/');
        normalised.append(name.data(), name.size());
    }
    if (normalised.size() > MaxShmNameLen)
    {
        throw FormatError{"cme::ShmMemory: shm name too long"};
    }
    return normalised;
}

struct MappedRegion_t
{
    void* base;
    std::uint64_t size;
};

[[nodiscard]] MappedRegion_t openCreator(const std::string& normalised, std::uint64_t areaSize)
{
    const int file = ::shm_open(normalised.c_str(), O_CREAT | O_RDWR, 0600);
    if (file < 0)
    {
        throw FormatError{std::string{"cme::ShmMemory shm_open: "} + std::strerror(errno)};
    }
    void* mapped = MAP_FAILED;
    try
    {
        if (::ftruncate(file, static_cast<off_t>(areaSize)) < 0)
        {
            throw FormatError{std::string{"cme::ShmMemory ftruncate: "} +
                              std::strerror(errno)};
        }
        mapped = ::mmap(nullptr, areaSize, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
        if (mapped == MAP_FAILED)
        {
            throw FormatError{std::string{"cme::ShmMemory mmap: "} + std::strerror(errno)};
        }
    }
    catch (...)
    {
        if (mapped != MAP_FAILED)
        {
            ::munmap(mapped, areaSize);
        }
        ::close(file);
        // No unlink: shm name lifetime is the caller's responsibility.
        throw;
    }
    ::close(file);
    return {mapped, areaSize};
}

[[nodiscard]] MappedRegion_t openJoiner(const std::string& normalised)
{
    const int file = ::shm_open(normalised.c_str(), O_RDWR, 0);
    if (file < 0)
    {
        throw FormatError{std::string{"cme::ShmMemory shm_open(attach): "} +
                          std::strerror(errno)};
    }
    void* mapped = MAP_FAILED;
    std::uint64_t mapSize = 0;
    try
    {
        struct stat fileStat;
        if (::fstat(file, &fileStat) < 0)
        {
            throw FormatError{std::string{"cme::ShmMemory fstat: "} + std::strerror(errno)};
        }
        if (fileStat.st_size <= 0)
        {
            throw FormatError{"cme::ShmMemory: shm size invalid"};
        }
        mapSize = static_cast<std::uint64_t>(fileStat.st_size);
        mapped = ::mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
        if (mapped == MAP_FAILED)
        {
            throw FormatError{std::string{"cme::ShmMemory mmap(attach): "} +
                              std::strerror(errno)};
        }
    }
    catch (...)
    {
        ::close(file);
        throw;
    }
    ::close(file);
    return {mapped, mapSize};
}

}  // namespace

ShmMemory::ShmMemory(std::string_view name)
    : Memory{nullptr, 0}
{
    const auto mapping = openJoiner(normaliseShmName(name));
    base_ = mapping.base;
    mappedSize_ = mapping.size;
}

ShmMemory::ShmMemory(std::string_view name, std::uint64_t areaSize)
    : Memory{nullptr, 0}
{
    if (areaSize == 0)
    {
        throw FormatError{"cme::ShmMemory: creator areaSize must be > 0"};
    }
    const auto mapping = openCreator(normaliseShmName(name), areaSize);
    base_ = mapping.base;
    mappedSize_ = mapping.size;
}

}  // namespace cme
