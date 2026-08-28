// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/area.cpp -- see area.hpp.

#include "shared/area.hpp"

#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

#include "cmed/errors.hpp"
#include "shared/posix/mem_file.hpp"
#include "shared/posix/unique_fd.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed
{

namespace
{

// The mapping's own failure is a POSIX one, and a requester holding this library's errors should not
// have to catch two hierarchies to learn that a descriptor could not be mapped.
[[nodiscard]] posix::MemFile map(posix::UniqueFd backing)
{
    try
    {
        return posix::MemFile::adopt(std::move(backing));
    }
    catch (const std::system_error& failure)
    {
        throw CmedBackendError{std::string{"cmed::CmedArea "} + failure.what(), failure.code()};
    }
}

}  // namespace

CmedArea::CmedArea(posix::MemFile file, protocol::SharedArea_t* area) noexcept
    : file_{std::move(file)},
      area_{area}
{
}

CmedArea CmedArea::adopt(posix::MemFile file, protocol::SharedArea_t* area) noexcept
{
    return CmedArea{std::move(file), area};
}

CmedArea CmedArea::attach(posix::UniqueFd backing)
{
    posix::MemFile file = map(std::move(backing));

    // No placement new: the object is the daemon's, and starting a second lifetime over it here would
    // zero what every other requester is using.
    auto* const area = static_cast<protocol::SharedArea_t*>(file.base());

    const std::uint32_t version = area->getAbiVersion();
    if (version == 0)
    {
        throw CmedAreaNotReadyError{"cmed::CmedArea: the descriptor carries no published area"};
    }
    if (version != protocol::AbiVersion)
    {
        throw CmedAreaInvalidError{"cmed::CmedArea: the area is abi " + std::to_string(version) +
                                   ", this build speaks " + std::to_string(protocol::AbiVersion)};
    }

    // One version can still disagree about size when a build was patched without bumping it, and the
    // requester's own sizeof is the only number it can check that against.
    const std::uint32_t stated = area->getAreaBytes();
    if (stated != sizeof(protocol::SharedArea_t))
    {
        throw CmedAreaInvalidError{"cmed::CmedArea: the area is " + std::to_string(stated) +
                                   " bytes, this build lays out " + std::to_string(sizeof(protocol::SharedArea_t))};
    }

    return CmedArea{std::move(file), area};
}

}  // namespace cmed
