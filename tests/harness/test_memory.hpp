// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_memory.hpp -- one named area on the medium, owned by one run.
//
// Every test and every probe needs the same three things from the medium and gets them
// nowhere else: a name derived from config.yaml and the run's own identity, a clean start
// on that name, and its removal afterwards. cme::Memory maps a URI and never removes one,
// because removal is an administrative act rather than a mapping: a public
// Memory::remove(uri) would let any caller destroy a region other peers still hold.
//
// What differs between callers is only who maps it. A case hands uri() to cme::Session or
// cme::Geometry and lets the library map. A probe measures the medium itself and asks for
// map(). Both get the same naming, the same clean start, and the same removal.

#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "cme/shared.hpp"
#include "config_reader.hpp"
#include "core/layout/geometry.hpp"
#include "core/types.hpp"
#include "memory/memory.hpp"

namespace harness
{

enum class Backend
{
    Shm,
    Dax,
    Uc
};

// One rule for every POSIX shm name this harness makes: what the run calls itself, what the
// area is for, and the pid that separates two runs of the same registration -- which is what
// an overlapping sweep launches.
[[nodiscard]] inline std::string shmName(const std::string& runName, const std::string& label)
{
    return "/cme_" + runName + (label.empty() ? std::string{} : "_" + label) + "_" +
           std::to_string(::getpid());
}

// What --backend names. Anything unrecognised is shm, which every machine has.
[[nodiscard]] inline Backend backendFromName(const std::string& name) noexcept
{
    if (name == "dax")
    {
        return Backend::Dax;
    }
    if (name == "uc")
    {
        return Backend::Uc;
    }
    return Backend::Shm;
}

// The medium a run asked for is not on this machine, so there is nothing to run on. Thrown
// rather than exited so the exit code is decided in one place.
class MediumUnavailable : public std::runtime_error
{
public:
    explicit MediumUnavailable(const std::string& why)
        : std::runtime_error(why)
    {
    }
};

class TestMemory
{
public:
    // What a devdax node hands out at a time, and so what a caller gets: 2 MiB is the
    // devdax fault granularity, which is also the smallest mapping the kernel grants, and
    // it is above the largest region this codebase can format (~1.03 MiB at the
    // MaxDomains/MaxPeers ceiling of 64). One window per concurrent run, so two never
    // share a byte.
    static constexpr std::uint64_t WindowBytes = 2 * MiB;

    virtual ~TestMemory() = default;

    TestMemory(const TestMemory&) = delete;
    TestMemory& operator=(const TestMemory&) = delete;

    // Builds the medium @backend names, or throws MediumUnavailable when this machine
    // cannot provide it. A constructed one is a medium that is present, named, and empty.
    // @objectOverride names the backend object outright, for a probe pointed somewhere by
    // hand. Left empty, the name comes from the config and @runName.
    [[nodiscard]] static std::unique_ptr<TestMemory> open(const ConfigReader& config,
                                                          Backend backend,
                                                          const std::string& runName,
                                                          std::uint64_t slot = 0,
                                                          const std::string& objectOverride = {});

    // ── naming ─────────────────────────────────────────────────────────

    // What cme::Session::format, cme::Geometry::create and cme::Memory take.
    [[nodiscard]] virtual std::string uri() const
    {
        return scheme() + object_;
    }

    // A second area on the same medium, for a run that sweeps a parameter and wants one
    // per point.
    [[nodiscard]] virtual std::string uriFor(const std::string& suffix) const
    {
        return scheme() + object_ + "_" + suffix;
    }

    // The backend's own name for the area, for a log line.
    [[nodiscard]] const std::string& name() const noexcept
    {
        return object_;
    }

    // Derived from the medium, not chosen: a uc mount is uncacheable and a dax slot needs
    // explicit writeback, whatever a caller might prefer.
    [[nodiscard]] virtual cme::CoherencyMode coherency() const noexcept = 0;

    // ── region views, for a case ───────────────────────────────────────

    // Formats the area and hands back the region. The geometry is the case's decision, so
    // it passes it in rather than this holding one.
    [[nodiscard]] cme::Geometry createRegion(cme::DomainId domainCeiling, cme::PeerId maxPeers,
                                             const cme::Geometry::FormatOpts_t& opts) const
    {
        return cme::Geometry::create(uri(), domainCeiling, maxPeers, opts);
    }

    // A second view of an area that is already formatted, for a case that reaches past the
    // public API to inject a fault or read a member slot. Separate from the view the code
    // under test holds, so the two are told apart in the source.
    [[nodiscard]] cme::Geometry openRegion() const
    {
        return cme::Geometry::open(uri());
    }

    // ── raw bytes, for a probe ─────────────────────────────────────────

    // Maps @bytes of the area and returns its base. Goes through cme::Memory so the tree
    // has one implementation of the open/ftruncate/mmap rules each backend imposes.
    [[nodiscard]] void* map(std::uint64_t bytes)
    {
        mapping_ = cme::Memory::create(uri(), bytes);
        return mapping_->getBase();
    }

    [[nodiscard]] void* getBase() const noexcept
    {
        return (mapping_ != nullptr) ? mapping_->getBase() : nullptr;
    }

    // Take the area away now, for a run that formats it more than once. The destructor
    // does this too; calling it twice is not an error.
    void remove() const noexcept
    {
        clear();
    }

    [[nodiscard]] std::uint64_t getMappedSize() const noexcept
    {
        return (mapping_ != nullptr) ? mapping_->getMappedSize() : 0;
    }

protected:
    explicit TestMemory(std::string object)
        : object_{std::move(object)},
          owner_{::getpid()}
    {
    }

    // Every backend calls this from its own constructor and its own destructor. It cannot
    // live in this class's constructor or destructor: there the derived part is not yet
    // built, or already gone, and removeObject() would not dispatch.
    //
    // A forked child unwinds through the destructor too, and removing there would take the
    // area out from under the parent that is still running on it.
    void clear() const noexcept
    {
        if (::getpid() == owner_)
        {
            removeObject();
        }
    }

    [[nodiscard]] virtual const char* scheme() const noexcept = 0;

    // Best-effort: absence is not an error.
    virtual void removeObject() const noexcept = 0;

    std::string object_;  // the backend's own name: a shm name, a file path, a device path
    ::pid_t owner_;

private:
    std::unique_ptr<cme::Memory> mapping_;
};

// A POSIX shared-memory segment. Present on every machine, so nothing to refuse.
class ShmTestMemory : public TestMemory
{
public:
    // The pid separates two runs of the same name, which is what an overlapping sweep
    // launches; @runName separates one registration from another.
    ShmTestMemory(const std::string& runName, std::uint64_t, const std::string& objectOverride)
        : TestMemory{objectOverride.empty() ? shmName(runName, {}) : objectOverride}
    {
        clear();
    }

    ~ShmTestMemory() override
    {
        clear();
    }

    [[nodiscard]] cme::CoherencyMode coherency() const noexcept override
    {
        return cme::CoherencyMode::CacheCoherent;
    }

protected:
    [[nodiscard]] const char* scheme() const noexcept override
    {
        return "shm:";
    }

    void removeObject() const noexcept override
    {
        (void)::shm_unlink(object_.c_str());
    }
};

// A file on an uncacheable mount.
class FileTestMemory : public TestMemory
{
public:
    FileTestMemory(const ConfigReader& config, const std::string& runName, std::uint64_t,
                   const std::string& objectOverride)
        : TestMemory{objectOverride.empty() ? mountedDir(config) + "/cme_" + runName
                                            : objectOverride}
    {
        clear();
    }

    ~FileTestMemory() override
    {
        clear();
    }

    [[nodiscard]] cme::CoherencyMode coherency() const noexcept override
    {
        return cme::CoherencyMode::Uncached;
    }

protected:
    [[nodiscard]] const char* scheme() const noexcept override
    {
        return "file:";
    }

    void removeObject() const noexcept override
    {
        (void)std::remove(object_.c_str());
    }

private:
    // Absent key and downed mount read the same to a run that wanted the mount, so one
    // message covers both and names the file to fix.
    static std::string mountedDir(const ConfigReader& config)
    {
        const std::string& dir = config.site().fileBackendDir;
        if (dir.empty())
        {
            throw MediumUnavailable{"no mounted file_backend_dir per " + config.path()};
        }
        return dir;
    }
};

// A window on a devdax device. The slot picks the window, so two dax runs never share a
// byte, and config_reader turns that index into the offset.
class DaxTestMemory : public TestMemory
{
public:
    DaxTestMemory(const ConfigReader& config, const std::string&, std::uint64_t slot,
                  const std::string& objectOverride)
        : TestMemory{objectOverride.empty() ? device(config) : objectOverride},
          offset_{windowOffset(config.site(), object_, slot)}
    {
        clear();
    }

    ~DaxTestMemory() override
    {
        clear();
    }

    [[nodiscard]] std::string uri() const override
    {
        return std::string{"dax:"} + object_ + "@" + std::to_string(offset_);
    }

    // One window per slot: a sweep reuses it point by point rather than naming a variant,
    // because the device hands out windows and a name cannot make another one.
    [[nodiscard]] std::string uriFor(const std::string&) const override
    {
        return uri();
    }

    [[nodiscard]] cme::CoherencyMode coherency() const noexcept override
    {
        return cme::CoherencyMode::Flush;
    }

protected:
    [[nodiscard]] const char* scheme() const noexcept override
    {
        return "dax:";
    }

    // The window is part of the device. Nothing creates or destroys it, and a format
    // overwrites it in place.
    void removeObject() const noexcept override
    {
    }

private:
    static std::string device(const ConfigReader& config)
    {
        const std::string& node = config.site().daxDevice;
        if (node.empty())
        {
            throw MediumUnavailable{"no usable dax_device per " + config.path()};
        }
        return node;
    }

    // dax_slot_base pins window 0 outright, which is the answer for a device whose sysfs
    // size node is absent, or one whose filesystem reaches further than the reserve allows
    // for. Left unset, window 0 lands one reserve short of the end.
    //
    // Refuses rather than returning a bad offset: the caller writes to what it gets back,
    // and offset 0 on a device that carries a filesystem is that filesystem.
    static std::uint64_t windowOffset(const ConfigReader::Site_t& site,
                                      const std::string& node, std::uint64_t slot)
    {
        const std::uint64_t size = deviceSize(node);
        std::uint64_t base = site.daxSlotBase;
        if (base == 0)
        {
            if (size <= site.daxSlotReserve)
            {
                throw MediumUnavailable{
                    node + ": size " + std::to_string(size) + " <= reserve " +
                    std::to_string(site.daxSlotReserve) +
                    ". Set dax_slot_base clear of any filesystem on the device, or lower "
                    "dax_slot_reserve."};
            }
            base = ((size - site.daxSlotReserve) / WindowBytes) * WindowBytes;
        }

        const std::uint64_t offset = base + slot * WindowBytes;
        if (size != 0 && offset + WindowBytes > size)
        {
            throw MediumUnavailable{node + ": slot " + std::to_string(slot) +
                                    " runs past the end; raise dax_slot_reserve"};
        }
        return offset;
    }

    // Size from sysfs, or 0 when the node does not export one. Read here rather than in
    // ConfigReader: --target may name a device the config never mentioned, and sysfs is not
    // config.yaml.
    static std::uint64_t deviceSize(const std::string& node)
    {
        const std::size_t slash = node.find_last_of('/');
        const std::string leaf = (slash == std::string::npos) ? node : node.substr(slash + 1);

        std::FILE* file = std::fopen(("/sys/bus/dax/devices/" + leaf + "/size").c_str(), "r");
        if (file == nullptr)
        {
            return 0;
        }
        std::uint64_t bytes = 0;
        const int fields = std::fscanf(file, "%" SCNu64, &bytes);
        std::fclose(file);
        return (fields == 1) ? bytes : 0;
    }

    std::uint64_t offset_;
};

inline std::unique_ptr<TestMemory> TestMemory::open(const ConfigReader& config, Backend backend,
                                                    const std::string& runName,
                                                    std::uint64_t slot,
                                                    const std::string& objectOverride)
{
    switch (backend)
    {
        case Backend::Dax:
            return std::make_unique<DaxTestMemory>(config, runName, slot, objectOverride);
        case Backend::Uc:
            return std::make_unique<FileTestMemory>(config, runName, slot, objectOverride);
        default:
            return std::make_unique<ShmTestMemory>(runName, slot, objectOverride);
    }
}

}  // namespace harness
