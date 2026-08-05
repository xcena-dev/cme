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
// below speaks the permission ioctl of an internal, unreleased filesystem. The
// constants are open-coded rather than included, so this compiles and runs
// anywhere -- any other mount answers ENOTTY or EINVAL and the call is skipped.
// That makes the coupling invisible at build time, which is exactly why it is
// written down here.
//
// For a file on a marufs/devdax mount this hits the same f_op->mmap path as
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

struct Mapping_t
{
    void* base;
    std::uint64_t size;
};

// O_CLOEXEC required: marufs_mmap rejects non-cloexec fds (-EACCES).
[[nodiscard]] int openFile(const std::string& path, bool create)
{
    const int flags = (create ? (O_CREAT | O_RDWR) : O_RDWR) | O_CLOEXEC;
    const int file = ::open(path.c_str(), flags, 0644);
    if (file < 0)
    {
        const auto failure = lastSystemError();
        throw BackendError{"cme::FileMemory open(" + path + ")", failure};
    }
    return file;
}

// marufs authorizes mmap per process, and a freshly created file's RAT entry has
// default_perms=0, so every peer other than the creator is denied (kernel/src/acl.c). cme
// peers are separate processes, so the creator opens the default up once. Mirrors
// marufs_uapi.h: MARUFS_IOC_PERM_SET_DEFAULT / MARUFS_PERM_ALL. node_id and pid are for
// per-target PERM_GRANT and unread here.
struct MarufsPermReq_t
{
    std::uint32_t nodeId;
    std::uint32_t pid;
    std::uint32_t perms;
    std::uint32_t reserved;
};
constexpr std::uint32_t MarufsPermAll = 0x003FU;

// ENOTTY/EINVAL means the mount does not know the ioctl (tmpfs and friends), which is fine.
// Anything else would surface later as an unexplained mmap EACCES in another process.
void grantDefaultPerms(int file, const std::string& path)
{
    MarufsPermReq_t req{};
    req.perms = MarufsPermAll;
    if (::ioctl(file, _IOW('X', 11, MarufsPermReq_t), &req) == 0 || errno == ENOTTY ||
        errno == EINVAL)
    {
        return;
    }
    const auto failure = lastSystemError();
    ::close(file);
    throw BackendError{"cme::FileMemory perm_set_default(" + path + ")", failure};
}

// Consumes @file either way.
[[nodiscard]] void* mapFd(int file, const std::string& path, std::uint64_t mapSize)
{
    void* mapped = ::mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
    // Before the close, which is allowed to leave its own value in errno.
    const auto failure = (mapped == MAP_FAILED) ? lastSystemError() : std::error_code{};
    ::close(file);
    if (mapped == MAP_FAILED)
    {
        throw BackendError{"cme::FileMemory mmap(" + path + ")", failure};
    }
    return mapped;
}

[[nodiscard]] Mapping_t openCreator(std::string_view pathView, std::uint64_t areaSize)
{
    const std::string path{pathView};
    const std::uint64_t mapSize = roundUp(areaSize, PmdAlign);
    const int file = openFile(path, /*create=*/true);
    // WORM: ftruncate only grows a fresh file. A pre-sized file (created and held by another
    // process) is already finalized and rejects re-truncation, so skip when it is big enough.
    struct stat info = {};
    const bool needGrow =
        ::fstat(file, &info) != 0 || static_cast<std::uint64_t>(info.st_size) < mapSize;
    if (needGrow && ::ftruncate(file, static_cast<off_t>(mapSize)) != 0)
    {
        const auto failure = lastSystemError();
        ::close(file);
        throw BackendError{"cme::FileMemory ftruncate(" + path + ")", failure};
    }
    grantDefaultPerms(file, path);
    return {mapFd(file, path, mapSize), mapSize};
}

// Map what the file actually holds. The creator sized it to the region, which may be more
// than one PMD, and a joiner that guessed PmdAlign would under-map and fail to bind.
[[nodiscard]] Mapping_t openJoiner(std::string_view pathView)
{
    const std::string path{pathView};
    const int file = openFile(path, /*create=*/false);
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
    return {mapFd(file, path, mapSize), mapSize};
}

}  // namespace

FileMemory::FileMemory(std::string_view path)
    : Memory{nullptr, 0}
{
    const auto mapping = openJoiner(path);
    base_ = mapping.base;
    mappedSize_ = mapping.size;
}

FileMemory::FileMemory(std::string_view path, std::uint64_t areaSize)
    : Memory{nullptr, 0}
{
    const auto mapping = openCreator(path, areaSize);
    base_ = mapping.base;
    mappedSize_ = mapping.size;
}

}  // namespace cme
