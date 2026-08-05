// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// memory.hpp -- mmap RAII bytes. Layout interpretation lives in Geometry.
//
// DaxMemory: /dev/dax* CXL FAM, noncoherent wmb/rmb regime (SWOT is a necessity).
// ShmMemory: POSIX shm_open, coherent regime (SWOT is a locality optimisation).
// shm names are never auto-unlinked; callers shm_unlink explicitly.

#pragma once

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string_view>
#include <system_error>

namespace cme
{

// PMD size. dax and marufs-backed file mappings must be a multiple of it; POSIX shm is not
// constrained this way, so ShmMemory does not use it.
inline constexpr std::uint64_t PmdAlign = 2ULL * 1024 * 1024;

// What the failing call left in errno, as the code a BackendError carries. Call it before anything
// else runs: a string concatenation between the failing call and here may overwrite errno.
[[nodiscard]] inline std::error_code lastSystemError() noexcept
{
    return std::error_code{errno, std::generic_category()};
}

class Memory
{
public:
    // ── rule of five ───────────────────────────────────────────────
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    Memory(Memory&&) = delete;
    Memory& operator=(Memory&&) = delete;
    // Unmaps whatever the derived ctor mapped -- every backend's teardown is the same
    // munmap, so none of them declares a destructor.
    virtual ~Memory();

    // ── factories ──────────────────────────────────────────────────
    // URI: "<scheme>:<path>", scheme picks backend (dax / shm). dax accepts a
    // "@<offset>" suffix (decimal or 0x, PMD-aligned) to place the region inside the
    // device, so disjoint regions can share one /dev/dax.
    [[nodiscard]] static std::unique_ptr<Memory> open(std::string_view uri);
    // dax rounds up to PMD; shm ftruncates to areaSize.
    [[nodiscard]] static std::unique_ptr<Memory>
    create(std::string_view uri, std::uint64_t areaSize);

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] void* getBase() const noexcept
    {
        return base_;
    }
    [[nodiscard]] std::uint64_t getMappedSize() const noexcept
    {
        return mappedSize_;
    }

protected:
    Memory(void* base, std::uint64_t mappedSize) noexcept
        : base_{base},
          mappedSize_{mappedSize}
    {
    }

    void* base_;
    std::uint64_t mappedSize_;
};

// @offset places the mapping inside the device (PMD-aligned, 0 = device start). Two
// DaxMemory over one device with offsets a PMD apart share no byte.
class DaxMemory : public Memory
{
public:
    // ── rule of five ───────────────────────────────────────────────
    explicit DaxMemory(std::string_view path, std::uint64_t offset = 0);
    DaxMemory(std::string_view path, std::uint64_t areaSize, std::uint64_t offset);
    DaxMemory(const DaxMemory&) = delete;
    DaxMemory& operator=(const DaxMemory&) = delete;
    DaxMemory(DaxMemory&&) = delete;
    DaxMemory& operator=(DaxMemory&&) = delete;
};

// Regular file (O_CREAT + ftruncate + mmap). On a marufs/devdax mount this
// uses the same kernel mmap path as dax, inheriting its pgprot (e.g. UC).
class FileMemory : public Memory
{
public:
    // ── rule of five ───────────────────────────────────────────────
    explicit FileMemory(std::string_view path);
    FileMemory(std::string_view path, std::uint64_t areaSize);
    FileMemory(const FileMemory&) = delete;
    FileMemory& operator=(const FileMemory&) = delete;
    FileMemory(FileMemory&&) = delete;
    FileMemory& operator=(FileMemory&&) = delete;
};

class ShmMemory : public Memory
{
public:
    // ── rule of five ───────────────────────────────────────────────
    explicit ShmMemory(std::string_view name);
    ShmMemory(std::string_view name, std::uint64_t areaSize);
    ShmMemory(const ShmMemory&) = delete;
    ShmMemory& operator=(const ShmMemory&) = delete;
    ShmMemory(ShmMemory&&) = delete;
    ShmMemory& operator=(ShmMemory&&) = delete;
};

}  // namespace cme
