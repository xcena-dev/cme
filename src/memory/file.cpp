// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// file.cpp -- regular-file backend (open O_CREAT + ftruncate + mmap).
//
// STATUS: pre-release, and used chiefly for development and testing. The
// production CXL path is the devdax backend; this one exists so the suite can
// exercise an uncacheable mapping, and so a plain file can stand in for FAM on a
// machine with no device. Treat its behaviour as provisional.
//
// It also carries one dependency the other backends do not: grantDefaultPerms()
// and the cache-pool calls below speak the ioctls of one filesystem. The
// constants are open-coded rather than included, so this compiles and runs
// anywhere -- any other mount answers ENOTTY or EINVAL and the call is skipped.
// That makes the coupling invisible at build time, which is exactly why it is
// written down here.
//
// For a file on a CXL filesystem mount this hits the same f_op->mmap path as
// dax, so the kernel's pgprot (e.g. UC) applies. Sizes via ftruncate only when
// the file is smaller than needed -- a pre-sized file held by another process
// (WORM-finalized) is mapped as-is.

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "memory/memory.hpp"
#include "util/util.hpp"

namespace cme
{

namespace
{

struct Mapping_t
{
    void* base;
    std::uint64_t size;
    std::optional<CoherencyMode> pool;  // the mode the region's pool calls for
};

// O_CLOEXEC required: the CXL filesystem refuses mmap, read and its ioctls on an fd without it.
[[nodiscard]] std::int32_t openFile(const std::string& path, bool create, bool writable)
{
    const std::int32_t access = writable ? O_RDWR : O_RDONLY;
    const std::int32_t flags = (create ? (O_CREAT | access) : access) | O_CLOEXEC;
    const auto file = ::open(path.c_str(), flags, 0644);
    if (file < 0)
    {
        const auto failure = lastSystemError();
        throw BackendError{"cme::FileMemory open(" + path + ")", failure};
    }
    return file;
}

// The filesystem authorizes mmap per process, and a freshly created file grants nothing by
// default, so every peer other than the creator is denied. cme peers are separate processes, so
// the creator opens the default up once. Mirrors FS_IOC_PERM_SET_DEFAULT and its fs_perm_req.
// uid and gid name a target for PERM_GRANT and are unread here.
struct FsPermReq_t
{
    static constexpr std::uint32_t Read = 0x0001U;
    static constexpr std::uint32_t Write = 0x0002U;

    std::uint32_t uid;
    std::uint32_t gid;
    std::uint32_t perms;
    std::uint32_t reserved;
};

// @wanted is what the mapping below asks for and no more: DELETE, ADMIN or GRANT in a default
// would hand every process on the host what one mapping never uses.
// ENOTTY/EINVAL means the mount does not know the ioctl (tmpfs and friends), which is fine.
// EACCES means this entry is not ours to configure: a file another process created, or one the
// filesystem itself made and left ownerless. Its defaults are already whatever they are, and the
// mmap below is what decides whether they suffice.
// Anything else would surface later as an unexplained mmap EACCES in another process.
void grantDefaultPerms(std::int32_t file, const std::string& path, std::uint32_t wanted)
{
    FsPermReq_t req{};
    req.perms = wanted;
    if (::ioctl(file, _IOW('X', 11, FsPermReq_t), &req) == 0 ||
        errno == ENOTTY || errno == EINVAL || errno == EACCES)
    {
        return;
    }
    const auto failure = lastSystemError();
    ::close(file);
    throw BackendError{"cme::FileMemory perm_set_default(" + path + ")", failure};
}

// Mirrors FS_IOC_CACHE_SET / FS_IOC_CACHE_GET and their fs_cache_req.
struct FsCacheReq_t
{
    static constexpr std::uint32_t WriteBack = 0U;
    static constexpr std::uint32_t Uncached = 1U;

    std::uint32_t policy;
    std::uint32_t reserved;
};

// Before ftruncate, which places the region and so fixes its pool for good.
// ENOTTY/EINVAL means the mount maps every file one way and has no pool to pick.
void setCachePolicy(std::int32_t file, const std::string& path, CoherencyMode coherency)
{
    FsCacheReq_t req{};
    req.policy = (coherency == CoherencyMode::Uncached) ? FsCacheReq_t::Uncached : FsCacheReq_t::WriteBack;
    if (::ioctl(file, _IOW('X', 13, FsCacheReq_t), &req) == 0 || errno == ENOTTY || errno == EINVAL)
    {
        return;
    }
    const auto failure = lastSystemError();
    ::close(file);
    throw BackendError{"cme::FileMemory cache_set(" + path + ")", failure};
}

[[nodiscard]] std::optional<CoherencyMode> getCachePolicy(std::int32_t file) noexcept
{
    FsCacheReq_t req{};
    if (::ioctl(file, _IOR('X', 14, FsCacheReq_t), &req) != 0)
    {
        return std::nullopt;
    }
    return (req.policy == FsCacheReq_t::Uncached) ? CoherencyMode::Uncached : CoherencyMode::Flush;
}

// Consumes @file either way.
[[nodiscard]] void* mapFd(std::int32_t file, const std::string& path, std::uint64_t mapSize, bool writable)
{
    const std::int32_t protection = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
    void* mapped = ::mmap(nullptr, mapSize, protection, MAP_SHARED, file, 0);
    // Before the close, which is allowed to leave its own value in errno.
    const auto failure = (mapped == MAP_FAILED) ? lastSystemError() : std::error_code{};
    ::close(file);
    if (mapped == MAP_FAILED)
    {
        throw BackendError{"cme::FileMemory mmap(" + path + ")", failure};
    }
    return mapped;
}

[[nodiscard]] Mapping_t openCreator(std::string_view pathView, std::uint64_t areaSize, CoherencyMode coherency)
{
    const std::string path{pathView};
    const std::uint64_t mapSize = roundUp(areaSize, PmdAlign);
    const auto file = openFile(path, /*create=*/true, /*writable=*/true);
    // WORM: ftruncate only grows a fresh file. A pre-sized file (created and held by another
    // process) is already finalized and rejects re-truncation, so skip when it is big enough.
    struct stat info = {};
    const bool needGrow = ::fstat(file, &info) != 0 ||
                          static_cast<std::uint64_t>(info.st_size) < mapSize;
    if (needGrow)
    {
        setCachePolicy(file, path, coherency);
        if (::ftruncate(file, static_cast<off_t>(mapSize)) != 0)
        {
            const auto failure = lastSystemError();
            ::close(file);
            throw BackendError{"cme::FileMemory ftruncate(" + path + ")", failure};
        }
    }
    grantDefaultPerms(file, path, FsPermReq_t::Read | FsPermReq_t::Write);
    // A pre-sized file kept the pool it was placed in, whatever @coherency asked for.
    const auto placed = getCachePolicy(file);
    return {mapFd(file, path, mapSize, /*writable=*/true), mapSize, placed};
}

// Map what the file actually holds. The creator sized it to the region, which may be more
// than one PMD, and a joiner that guessed PmdAlign would under-map and fail to bind.
// @writable false reads the header off a region that grants read and nothing else.
[[nodiscard]] Mapping_t openJoiner(std::string_view pathView, bool writable)
{
    const std::string path{pathView};
    const auto file = openFile(path, /*create=*/false, writable);
    struct stat info = {};
    if (::fstat(file, &info) != 0)
    {
        const auto failure = lastSystemError();
        ::close(file);
        throw BackendError{"cme::FileMemory fstat(" + path + ")", failure};
    }
    // roundUp turns a zero size into a whole PMD, so mmap would succeed over a file holding no
    // bytes and the first read would SIGBUS. ShmMemory's joiner refuses the same case, and as
    // there no syscall failed, so this carries no code.
    if (info.st_size <= 0)
    {
        ::close(file);
        throw BackendError{"cme::FileMemory: file size invalid (" + path + ")"};
    }
    const std::uint64_t mapSize = roundUp(static_cast<std::uint64_t>(info.st_size), PmdAlign);
    const auto placed = getCachePolicy(file);
    return {mapFd(file, path, mapSize, writable), mapSize, placed};
}

}  // namespace

FileMemory::FileMemory(std::string_view path)
    : Memory{nullptr, 0}
{
    const auto mapping = openJoiner(path, /*writable=*/true);
    base_ = mapping.base;
    mappedSize_ = mapping.size;
    poolCoherency_ = mapping.pool;
}

FileMemory::FileMemory(std::string_view path, ReadOnlyTag)
    : Memory{nullptr, 0}
{
    const auto mapping = openJoiner(path, /*writable=*/false);
    base_ = mapping.base;
    mappedSize_ = mapping.size;
    poolCoherency_ = mapping.pool;
}

FileMemory::FileMemory(std::string_view path, std::uint64_t areaSize, CoherencyMode coherency)
    : Memory{nullptr, 0}
{
    const auto mapping = openCreator(path, areaSize, coherency);
    base_ = mapping.base;
    mappedSize_ = mapping.size;
    poolCoherency_ = mapping.pool;
}

std::optional<CoherencyMode> FileMemory::readPoolCoherency(std::string_view path)
{
    const std::string named{path};
    const auto file = ::open(named.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0)
    {
        return std::nullopt;
    }
    const auto placed = getCachePolicy(file);
    ::close(file);
    return placed;
}

}  // namespace cme
