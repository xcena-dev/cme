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
// It also reaches the permission and cache-pool verbs of one filesystem through
// that filesystem's library, loaded when first needed. Without its header at build
// time, the library at run time, or on another mount, those calls are skipped.
//
// For a file on a CXL filesystem mount this hits the same f_op->mmap path as
// dax, so the kernel's pgprot (e.g. UC) applies. Sizes via ftruncate only when
// the file is smaller than needed -- a pre-sized file held by another process
// (WORM-finalized) is mapped as-is.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#ifdef CME_HAVE_ROOF_ABI
#include <dlfcn.h>

#include "roof/abi.hpp"
#include "roof/file.hpp"
#include "roof/soname.hpp"
#endif

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

#ifdef CME_HAVE_ROOF_ABI

// Null when the library is absent or does not serve the interface version this build was compiled
// against. Kept loaded for the life of the process, since the table's functions are called until exit.
[[nodiscard]] const roof::CApi_t* loadRoof()
{
    void* const library = ::dlopen(roof::SharedLibraryName, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        return nullptr;
    }
    const auto getTable = reinterpret_cast<roof::CApiGetFn>(::dlsym(library, FSUSER_C_API_SYMBOL));
    const roof::CApi_t* const table = (getTable != nullptr) ? getTable(roof::AbiVersion) : nullptr;
    if (table == nullptr)
    {
        ::dlclose(library);
    }
    return table;
}

[[nodiscard]] const roof::CApi_t* getRoofTable()
{
    static const roof::CApi_t* const Table = loadRoof();
    return Table;
}

constexpr roof::Permission ReadWrite = roof::Permission::Read | roof::Permission::Write;

// Each verb answers 0 or a negative errno, and -ENOTTY when the library is unusable, which is what
// a filesystem that does not know the ioctl answers too.
[[nodiscard]] std::int32_t setRoofDefaultPermission(std::int32_t file)
{
    const auto* const table = getRoofTable();
    return (table != nullptr) ? table->setDefaultPermission(file, ReadWrite) : -ENOTTY;
}

// The question mapping asks, put before mmap(2) as the library's own map does.
[[nodiscard]] std::int32_t askRoofPermission(std::int32_t file, bool writable)
{
    const auto* const table = getRoofTable();
    const auto asked = writable ? ReadWrite : roof::Permission::Read;
    return (table != nullptr) ? table->askPermission(file, asked) : -ENOTTY;
}

[[nodiscard]] std::int32_t setRoofCache(std::int32_t file, CoherencyMode coherency)
{
    const auto* const table = getRoofTable();
    const auto cache = (coherency == CoherencyMode::Uncached) ? roof::CachePolicy::Uncached : roof::CachePolicy::Writeback;
    const roof::RegionAttr_t attr{cache};
    return (table != nullptr) ? table->setRegionAttr(file, attr) : -ENOTTY;
}

[[nodiscard]] std::optional<CoherencyMode> readRoofCache(std::int32_t file)
{
    const auto* const table = getRoofTable();
    roof::RegionAttr_t attr;
    if (table == nullptr || table->readRegionAttr(file, &attr) != 0)
    {
        return std::nullopt;
    }
    return (attr.cache == roof::CachePolicy::Uncached) ? CoherencyMode::Uncached : CoherencyMode::Flush;
}

#else

[[nodiscard]] std::int32_t setRoofDefaultPermission(std::int32_t /*file*/)
{
    return -ENOTTY;
}

[[nodiscard]] std::int32_t askRoofPermission(std::int32_t /*file*/, bool /*writable*/)
{
    return -ENOTTY;
}

[[nodiscard]] std::int32_t setRoofCache(std::int32_t /*file*/, CoherencyMode /*coherency*/)
{
    return -ENOTTY;
}

[[nodiscard]] std::optional<CoherencyMode> readRoofCache(std::int32_t /*file*/)
{
    return std::nullopt;
}

#endif

// @result is an entry point's 0 or negative errno. A failure outside @tolerated closes @file and throws.
void requireResult(std::int32_t file, std::int32_t result, std::initializer_list<std::int32_t> tolerated,
                   std::string_view verb, const std::string& path)
{
    const auto isTolerated = [&](std::int32_t code)
    {
        return result == -code;
    };
    if (result == 0 || std::any_of(tolerated.begin(), tolerated.end(), isTolerated))
    {
        return;
    }
    const std::error_code failure{-result, std::system_category()};
    ::close(file);
    throw BackendError{std::string{"cme::FileMemory "}.append(verb).append("(").append(path).append(")"), failure};
}

// The filesystem denies every other peer's mmap of a fresh file, so the creator opens the default
// once, to what a mapping asks for and no more: DELETE, ADMIN or GRANT would reach every process.
void grantDefaultPerms(std::int32_t file, const std::string& path)
{
    // ENOTTY/EINVAL: the mount lacks the verb. EACCES: not our entry, so the mmap judges its default.
    // Anything else fails here rather than as an unexplained mmap EACCES in another process.
    requireResult(file, setRoofDefaultPermission(file), {ENOTTY, EINVAL, EACCES}, "perm_set_default", path);
}

// Before ftruncate, which places the region and so fixes its pool for good.
// ENOTTY/EINVAL means the mount maps every file one way and has no pool to pick.
void setCachePolicy(std::int32_t file, const std::string& path, CoherencyMode coherency)
{
    requireResult(file, setRoofCache(file, coherency), {ENOTTY, EINVAL}, "cache_set", path);
}

// Consumes @file either way.
[[nodiscard]] void* mapFd(std::int32_t file, const std::string& path, std::uint64_t mapSize, bool writable)
{
    requireResult(file, askRoofPermission(file, writable), {ENOTTY}, "perm_ask", path);
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
    grantDefaultPerms(file, path);
    // A pre-sized file kept the pool it was placed in, whatever @coherency asked for.
    const auto placed = readRoofCache(file);
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
    const auto placed = readRoofCache(file);
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
    const auto placed = readRoofCache(file);
    ::close(file);
    return placed;
}

}  // namespace cme
