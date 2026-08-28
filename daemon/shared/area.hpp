// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// shared/area.hpp -- the mapped, version-checked cmed shared area.
//
// Producing an area is the daemon's job (formatArea): it zeroes the area, so a requester must never
// call it on an area others are using.
//
// The area has no name. It is a memfd handed over on the socket, so only an admitted requester can
// reach it, and that requester can write any word in it.

#pragma once

#include "shared/posix/mem_file.hpp"
#include "shared/posix/unique_fd.hpp"
#include "shared/protocol/shared_area.hpp"

namespace cmed
{

class CmedArea
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────────
    CmedArea(const CmedArea&) = delete;
    CmedArea(CmedArea&&) noexcept = default;
    ~CmedArea() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────────
    CmedArea& operator=(const CmedArea&) = delete;
    CmedArea& operator=(CmedArea&&) noexcept = default;

    // ── factories ──────────────────────────────────────────────────────
    // Maps a descriptor and checks what was mapped; throws CmedAreaNotReadyError (unstamped) or
    // CmedAreaInvalidError (a version or size this build does not speak).
    [[nodiscard]] static CmedArea attach(posix::UniqueFd backing);

    // Takes ownership of a file the caller already made and stamped. Nothing to check here: the caller
    // wrote the header, and @area is the pointer whose lifetime its placement new began.
    [[nodiscard]] static CmedArea adopt(posix::MemFile file, protocol::SharedArea_t* area) noexcept;

    // ── accessors ──────────────────────────────────────────────────────
    [[nodiscard]] protocol::SharedArea_t& shared() noexcept
    {
        return *area_;
    }

    [[nodiscard]] const protocol::SharedArea_t& shared() const noexcept
    {
        return *area_;
    }

    // The backing memfd, for the daemon to attach to a Welcome. A requester keeps its own copy and
    // has nobody to hand it to, which costs one descriptor and leaves the seals readable.
    [[nodiscard]] posix::FileDesc descriptor() const noexcept
    {
        return file_.descriptor();
    }

private:
    CmedArea(posix::MemFile file, protocol::SharedArea_t* area) noexcept;

private:
    posix::MemFile file_;
    protocol::SharedArea_t* area_;
};

}  // namespace cmed
