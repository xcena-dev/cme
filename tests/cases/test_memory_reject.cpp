// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_memory_reject.cpp -- the half of the memory backends that refuses.
//
// The four backend files sit at 63-82% line coverage and what is missing in each is the same half:
// the failure branches. Every case in the tree opens a medium the harness already set up correctly,
// so a bad URI, a name that cannot exist, a size that cannot be mapped, and a devdax offset off its
// PMD boundary have never been through this code.
//
// The type is the assertion. InvalidArgumentError means the caller's own input cannot describe a
// region, so no retry helps; BackendError means the system refused, and its code() carries the
// errno the failing call left. threw<T> lets any other exception propagate, so a refusal for the
// wrong reason fails where it happened.
//
// Where the errno is decided by the kernel alone the case pins the std::errc value. Where it
// depends on the filesystem -- a size no mapping can hold gives up in ftruncate on one and in mmap
// on another -- only the presence of a code is asserted. Two refusals carry no code at all, because
// no syscall failed behind them: an shm name that exists unsized, and an empty file.
//
// Every assertion goes through a helper that takes a URI rather than a lambda. A lambda as the
// first argument of a nested call formats into four indented lines, which buries the one thing the
// line is about.
//
// The case builds its own URIs and its own scratch files rather than using ctx's medium, so it is
// registered on shm alone: a dax or uc variant would run byte-identical assertions.
//
// Two branches are deliberately out of reach and left uncovered. grantDefaultPerms in file.cpp only
// throws on a mount that answers its ioctl with something other than success, ENOTTY or EINVAL,
// which no ordinary filesystem does. DaxMemory's mmap failure needs a real device, and a case that
// requires one is a case that skips on most machines.

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>

#include "cme/errors.hpp"
#include "helper.hpp"
#include "memory/memory.hpp"
#include "test_context.hpp"
#include "test_memory.hpp"

namespace test
{
namespace
{

// Larger than any mapping this machine can hold, and small enough that rounding it up to a PMD
// boundary does not wrap.
constexpr std::uint64_t UnmappableSize = 1ULL << 62;

// harness::shmName owns the naming rule, pid included, so a leftover from a killed run cannot be
// mistaken for the real thing. Its leading slash is the shm name's; for the one scratch file below
// it doubles as the path separator after /tmp.
constexpr const char* RunName = "memory_reject";

// ── the two shapes every assertion below takes ──────────────────────

// Whether opening @uri was refused as the caller's own fault. Anything else propagates.
[[nodiscard]] bool refusesUri(const std::string& uri)
{
    return harness::threw<cme::InvalidArgumentError>(
        [&uri]()
        {
            (void)cme::Memory::open(uri);
        });
}

// nullopt when @body threw no BackendError. Engaged otherwise, holding the code it carried, which
// is empty for a refusal with no failing syscall behind it. Anything that is not a BackendError
// propagates, so a refusal of the wrong kind fails where it happened.
template <typename T_Body>
[[nodiscard]] std::optional<std::error_code> backendRefusal(T_Body body)
{
    try
    {
        body();
    }
    catch (const cme::BackendError& refused)
    {
        return refused.code();
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::error_code> openRefusal(const std::string& uri)
{
    return backendRefusal(
        [&uri]()
        {
            (void)cme::Memory::open(uri);
        });
}

[[nodiscard]] std::optional<std::error_code> createRefusal(const std::string& uri,
                                                           std::uint64_t areaSize)
{
    return backendRefusal(
        [&uri, areaSize]()
        {
            (void)cme::Memory::create(uri, areaSize);
        });
}

// @what is worded as the property being asserted, and the failing line says what arrived instead.
void checkCode(harness::TestContext& ctx, const char* what, std::errc expected,
               const std::optional<std::error_code>& refusal)
{
    if (!refusal)
    {
        ctx.checkf(false, "%s (nothing refused it)", what);
        return;
    }
    ctx.checkf(*refusal == expected, "%s (got %s)", what, refusal->message().c_str());
}

// For a refusal whose errno the filesystem decides.
void checkAnyCode(harness::TestContext& ctx, const char* what,
                  const std::optional<std::error_code>& refusal)
{
    ctx.checkf(refusal.has_value() && static_cast<bool>(*refusal), "%s", what);
}

// For a refusal with no failing syscall behind it, which is the one case code() is empty.
void checkNoCode(harness::TestContext& ctx, const char* what,
                 const std::optional<std::error_code>& refusal)
{
    ctx.checkf(refusal.has_value() && !static_cast<bool>(*refusal), "%s", what);
}

// ── the URI grammar (memory.cpp) ────────────────────────────────────

void checkUriGrammar(harness::TestContext& ctx)
{
    ctx.check(refusesUri("no-colon-at-all"), "URI without a scheme is refused");
    ctx.check(refusesUri(":/leading-colon"), "URI with an empty scheme is refused");
    ctx.check(refusesUri("shm:"), "URI with an empty path is refused");
    ctx.check(refusesUri("nvme:/dev/nvme0n1"), "open: an unknown scheme is refused");

    // create() answers the same way as open(), and repeats the scheme switch to do it.
    ctx.check(harness::threw<cme::InvalidArgumentError>(
                  []()
                  {
                      (void)cme::Memory::create("nvme:/dev/nvme0n1", 4096);
                  }),
              "create: an unknown scheme is refused");
}

// ── the dax offset suffix (memory.cpp + dax.cpp) ────────────────────
// None of the first three needs a device: the offset is parsed, then checked against the PMD
// boundary, before dax.cpp opens anything.

void checkDaxOffset(harness::TestContext& ctx)
{
    ctx.check(refusesUri("dax:/dev/dax0.0@notanumber"),
              "dax offset that is not a number is refused");
    ctx.check(refusesUri("dax:/dev/dax0.0@0x200000tail"),
              "dax offset with trailing junk is refused");

    // 4 KiB parses cleanly and is still not a PMD multiple, which dax.cpp checks itself so the
    // failure names the offset instead of surfacing as a bare mmap EINVAL.
    ctx.check(refusesUri("dax:/dev/dax0.0@4096"), "dax offset off its PMD boundary is refused");

    // A path that cannot exist, so this reaches dax.cpp's open() rather than its offset check.
    // Nothing creates it, so it needs no run-scoped name -- only to be absent.
    checkCode(ctx, "dax device that does not exist reports ENOENT",
              std::errc::no_such_file_or_directory,
              openRefusal("dax:/dev/cme_no_such_dax@0"));

    // The suffix is optional and means offset 0. The refusal being a BackendError rather than an
    // InvalidArgumentError is the assertion: the URI was well formed and the device was absent.
    checkCode(ctx, "dax URI with no offset suffix is well formed and means offset 0",
              std::errc::no_such_file_or_directory,
              openRefusal("dax:/dev/cme_no_such_dax"));

    // A '@' with nothing after it is the same case, and the '@' stays part of the device path
    // rather than being split off, so the name that reaches open() is the one written here.
    checkCode(ctx, "dax URI whose '@' carries no offset is well formed too",
              std::errc::no_such_file_or_directory,
              openRefusal("dax:/dev/cme_no_such_dax@"));
}

// ── the shm backend (shm.cpp) ───────────────────────────────────────

void checkShmBackend(harness::TestContext& ctx)
{
    const std::string name = harness::shmName(RunName, {});

    // Neither name rule is reachable through a URI: an empty path fails the grammar first, and a
    // name past NAME_MAX needs the class directly.
    ctx.check(harness::threw<cme::InvalidArgumentError>(
                  []()
                  {
                      const cme::ShmMemory unnamed{""};
                      (void)unnamed.getBase();
                  }),
              "shm: an empty name is refused");
    ctx.check(harness::threw<cme::InvalidArgumentError>(
                  []()
                  {
                      const cme::ShmMemory overlong{std::string(300, 'x')};
                      (void)overlong.getBase();
                  }),
              "shm: a name past NAME_MAX is refused");

    ctx.check(harness::threw<cme::InvalidArgumentError>(
                  [&name]()
                  {
                      (void)cme::Memory::create("shm:" + name, 0);
                  }),
              "shm: a creator asking for zero bytes is refused");

    checkCode(ctx, "shm: attaching to a name nobody made reports ENOENT",
              std::errc::no_such_file_or_directory,
              openRefusal("shm:" + harness::shmName(RunName, "absent")));

    // A name that exists but was never sized. shm_open succeeds and fstat reports zero, so the
    // refusal has no failing syscall behind it and carries no code.
    const int unsized = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    if (unsized >= 0)
    {
        static_cast<void>(::close(unsized));
        checkNoCode(ctx, "shm: attaching to an unsized name is refused, with no code",
                    openRefusal("shm:" + name));
        static_cast<void>(::shm_unlink(name.c_str()));
    }
    else
    {
        ctx.check(false, "shm: could not create an unsized name to attach to");
    }

    const std::string huge = harness::shmName(RunName, "huge");
    checkAnyCode(ctx, "shm: a size beyond the machine is refused",
                 createRefusal("shm:" + huge, UnmappableSize));
    static_cast<void>(::shm_unlink(huge.c_str()));
}

// ── the file backend (file.cpp) ─────────────────────────────────────

void checkFileBackend(harness::TestContext& ctx)
{
    // shmName's leading slash doubles as the separator after /tmp, so the pid rule stays in one
    // place for the one file this case actually creates.
    const std::string path = "/tmp" + harness::shmName(RunName, "file");

    // Nothing creates this one, so it needs no run-scoped name -- only to be unopenable.
    checkCode(ctx, "file: a path that cannot be opened reports ENOENT",
              std::errc::no_such_file_or_directory, openRefusal("file:/proc/cme/absent"));

    // An empty file is the case mmap will not catch: roundUp turns zero into a whole PMD, so the
    // mapping succeeds entirely past EOF and the first read SIGBUSes. The size check turns that
    // into a refusal, and like shm's it has no failing syscall behind it.
    const int empty = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (empty >= 0)
    {
        static_cast<void>(::close(empty));
        checkNoCode(ctx, "file: attaching to an empty file is refused, with no code",
                    openRefusal("file:" + path));
        static_cast<void>(::unlink(path.c_str()));
    }
    else
    {
        ctx.check(false, "file: could not create an empty file to attach to");
    }

    const std::string hugePath = path + "_huge";
    checkAnyCode(ctx, "file: a size beyond the machine is refused",
                 createRefusal("file:" + hugePath, UnmappableSize));
    static_cast<void>(::unlink(hugePath.c_str()));
}

}  // namespace

void runBody(harness::TestContext& ctx)
{
    checkUriGrammar(ctx);
    checkDaxOffset(ctx);
    checkShmBackend(ctx);
    checkFileBackend(ctx);
}

}  // namespace test

int main(int argc, char** argv)
{
    return harness::runCase(argc, argv, test::runBody);
}
