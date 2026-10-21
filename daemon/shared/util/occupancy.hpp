// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/util/occupancy.hpp -- one unit of a count another process reads.
//
// Returning it is a destructor because the scope it covers can throw, and a unit never given back
// is read forever after as demand that is not there.

#pragma once

#include <cstdint>

#include "common/ordered.hpp"

namespace cmed::util
{

class OccupancyToken
{
public:
    explicit OccupancyToken(ordered::SeqCst<std::uint32_t>& occupancy) noexcept
        : occupancy_{&occupancy}
    {
        occupancy_->add(1);
    }

    // One object is one unit. A second owner of the same one would hand back a unit it never took.
    OccupancyToken(const OccupancyToken&) = delete;
    OccupancyToken(OccupancyToken&&) = delete;
    OccupancyToken& operator=(const OccupancyToken&) = delete;
    OccupancyToken& operator=(OccupancyToken&&) = delete;

    ~OccupancyToken() noexcept
    {
        occupancy_->subtract(1);
    }

private:
    ordered::SeqCst<std::uint32_t>* occupancy_;
};

}  // namespace cmed::util
