// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// memory.cpp -- URI factories + move semantics for the Memory hierarchy.

#include "memory/memory.hpp"

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

#include "cme/errors.hpp"

namespace cme
{

namespace
{

struct UriParts_t
{
    std::string scheme;
    std::string path;
    std::uint64_t offset;  // dax only; 0 elsewhere
};

// "@<offset>" is split off for dax alone -- a shm name or file path may legitimately
// contain '@', and only dax can place a region partway into its backing object.
[[nodiscard]] UriParts_t parseUri(std::string_view uri)
{
    const auto pos = uri.find(':');
    if (pos == std::string_view::npos || pos == 0 || pos + 1 >= uri.size())
    {
        throw InvalidArgumentError{"cme: malformed URI (expected scheme:path)"};
    }
    UriParts_t parts{std::string{uri.substr(0, pos)}, std::string{uri.substr(pos + 1)}, 0};
    if (parts.scheme != "dax")
    {
        return parts;
    }
    const auto atSign = parts.path.rfind('@');
    if (atSign == std::string::npos || atSign + 1 >= parts.path.size())
    {
        return parts;
    }
    const std::string suffix = parts.path.substr(atSign + 1);
    std::size_t consumed = 0;
    try
    {
        parts.offset = std::stoull(suffix, &consumed, 0);
    }
    catch (const std::exception&)
    {
        throw InvalidArgumentError{std::string{"cme::Memory: malformed dax offset: "} + suffix};
    }
    if (consumed != suffix.size())
    {
        throw InvalidArgumentError{std::string{"cme::Memory: malformed dax offset: "} + suffix};
    }
    parts.path.resize(atSign);
    return parts;
}

}  // namespace

Memory::~Memory()
{
    if (base_ != nullptr && mappedSize_ > 0)
    {
        ::munmap(base_, mappedSize_);
        base_ = nullptr;
        mappedSize_ = 0;
    }
}

std::unique_ptr<Memory> Memory::open(std::string_view uri)
{
    const auto parts = parseUri(uri);
    if (parts.scheme == "dax")
    {
        return std::make_unique<DaxMemory>(parts.path, parts.offset);
    }
    if (parts.scheme == "shm")
    {
        return std::make_unique<ShmMemory>(parts.path);
    }
    if (parts.scheme == "file")
    {
        return std::make_unique<FileMemory>(parts.path);
    }
    throw InvalidArgumentError{std::string{"cme::Memory::open: unsupported scheme: "} +
                               parts.scheme};
}

std::unique_ptr<Memory> Memory::create(std::string_view uri, std::uint64_t areaSize)
{
    const auto parts = parseUri(uri);
    if (parts.scheme == "dax")
    {
        return std::make_unique<DaxMemory>(parts.path, areaSize, parts.offset);
    }
    if (parts.scheme == "shm")
    {
        return std::make_unique<ShmMemory>(parts.path, areaSize);
    }
    if (parts.scheme == "file")
    {
        return std::make_unique<FileMemory>(parts.path, areaSize);
    }
    throw InvalidArgumentError{std::string{"cme::Memory::create: unsupported scheme: "} +
                               parts.scheme};
}

}  // namespace cme
