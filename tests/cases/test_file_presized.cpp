// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_file_presized.cpp -- FileMemory's creator sizes a fresh file and leaves a sized one alone.
//
// openCreator calls ftruncate only when the file is smaller than the mapping it needs. The rule
// exists for a WORM mount: a file another process created and sized is finalized there, and a
// second ftruncate on it is refused, so a creator that always truncated could never join a
// region it did not make first.
//
// On an ordinary filesystem that skip is invisible, because the redundant ftruncate would
// succeed. So the file here is a memfd sealed with F_SEAL_SHRINK and sized above the mapping:
// truncating it down to the mapping size is refused the way the WORM mount refuses it, and the
// library reaches it through /proc/self/fd. A build that truncated unconditionally fails with
// EPERM, and the passing answer is the skip.
//
// The growing half is asserted on the same machinery with the seal left off, so the two answers
// differ only in the file the creator is handed.
//
// The case makes its own backing objects and never touches ctx's medium, so it is registered on
// shm alone: on any other backend it would run byte-identical assertions.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>

#include "memory/memory.hpp"
#include "test_context.hpp"

namespace test
{
namespace
{

// One PMD is the smallest area the file backend maps, and the mapping it computes for this is
// exactly PmdAlign. The pre-sized file is a PMD larger, so a truncate down to the mapping is a
// shrink and the seal refuses it.
constexpr std::uint64_t AreaSize = cme::PmdAlign;
constexpr std::uint64_t OversizeBytes = 2 * cme::PmdAlign;

// An anonymous file the case can size, seal and hand to the library by path. A memfd is the only
// ordinary-filesystem object that refuses a truncate the way a finalized WORM file does.
//
// A class rather than the bare open/close memory_reject uses, because the descriptor has to
// outlive the call under test: a memfd has no name on any filesystem, `/proc/self/fd/N` is the
// name, and that path resolves only while N is open. Memory::create throws, so the close belongs
// in a destructor.
class BackingFile
{
public:
    explicit BackingFile(const char* label)
        : file_{::memfd_create(label, MFD_CLOEXEC | MFD_ALLOW_SEALING)}
    {
    }

    ~BackingFile()
    {
        if (file_ >= 0)
        {
            static_cast<void>(::close(file_));
        }
    }

    BackingFile(const BackingFile&) = delete;
    BackingFile& operator=(const BackingFile&) = delete;

    [[nodiscard]] bool isOpen() const noexcept
    {
        return file_ >= 0;
    }

    [[nodiscard]] bool resize(std::uint64_t bytes) const noexcept
    {
        return ::ftruncate(file_, static_cast<off_t>(bytes)) == 0;
    }

    // Refuse any later truncate below the current size, which is what a finalized file does.
    [[nodiscard]] bool sealShrink() const noexcept
    {
        return ::fcntl(file_, F_ADD_SEALS, F_SEAL_SHRINK) == 0;
    }

    [[nodiscard]] std::uint64_t size() const noexcept
    {
        struct stat info = {};
        return ::fstat(file_, &info) == 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
    }

    // What cme::Memory::create takes. Opening this path yields another descriptor onto the same
    // object, so the library's own open/ftruncate/mmap sequence runs unchanged.
    [[nodiscard]] std::string uri() const
    {
        return "file:/proc/self/fd/" + std::to_string(file_);
    }

private:
    int file_;
};

// A pre-sized file is mapped as it stands. The seal is what makes the assertion mean something:
// without it, a creator that truncated anyway would pass this too.
void checkPresizedFileIsLeftAlone(harness::TestContext& ctx)
{
    const BackingFile backing{"cme_presized"};
    if (!ctx.check(backing.isOpen() && backing.resize(OversizeBytes) && backing.sealShrink(),
                   "presized: a sealed backing file of two PMDs is available"))
    {
        return;
    }

    const auto mapping = cme::Memory::create(backing.uri(), AreaSize);
    ctx.check(mapping->getMappedSize() == AreaSize,
              "presized: the creator maps the area without re-truncating the file");
    ctx.check(backing.size() == OversizeBytes,
              "presized: the file keeps the size its creator gave it");
}

// The other half of the same branch: a file smaller than the mapping is grown to it.
void checkFreshFileIsGrown(harness::TestContext& ctx)
{
    const BackingFile backing{"cme_fresh"};
    if (!ctx.check(backing.isOpen(), "fresh: a backing file is available"))
    {
        return;
    }

    const auto mapping = cme::Memory::create(backing.uri(), AreaSize);
    ctx.check(mapping->getMappedSize() == AreaSize, "fresh: the creator maps the area it asked for");
    ctx.check(backing.size() == AreaSize, "fresh: the creator grew the file to the mapping");
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    checkPresizedFileIsLeftAlone(ctx);
    checkFreshFileIsGrown(ctx);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
