// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// test_context.hpp -- what one run is, and the entry point that puts it in order.
//
// The order that matters is held here rather than repeated in every main(): flags before
// config, config before medium, medium before region, verdict after everything. Member
// declaration order is what enforces it, so a run cannot read a fact that is not settled.
//
// A case says what it does and nothing about how it starts or stops.

#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "cme/shared.hpp"
#include "config_reader.hpp"
#include "test_memory.hpp"
#include "test_options.hpp"
#include "tests/probe_context.hpp"

namespace harness
{

// A buffer the children of a run write and the parent reads: claim results, a barrier flag.
//
// A named POSIX shm segment rather than an anonymous mapping. Anonymous memory reaches a
// forked child and nothing else, because there is no name for another process to open.
//
// Host memory, whatever --backend the region sits on. The reporting path must not add
// traffic to the medium a case is measuring, and a barrier spinning on that medium would
// widen the very window a contention case is trying to narrow.
//
// Falsy when the mapping failed, with errno as the failing call left it. The caller decides
// what that means; nothing here prints or exits.
template <typename T>
class SharedBuffer
{
public:
    static_assert(std::is_trivially_copyable_v<T>,
                  "a shared buffer holds what both processes can read: no pointers, no vtable");

    SharedBuffer() = default;

    SharedBuffer(std::string name, std::uint64_t count)
        : count_{count},
          name_{std::move(name)},
          owner_{::getpid()}
    {
        const int file = ::shm_open(name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (file < 0)
        {
            return;
        }
        if (::ftruncate(file, static_cast<off_t>(bytes())) == 0)
        {
            void* page = ::mmap(nullptr, static_cast<std::size_t>(bytes()),
                                PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
            if (page != MAP_FAILED)
            {
                data_ = static_cast<T*>(page);
                std::memset(data_, 0, static_cast<std::size_t>(bytes()));
            }
        }
        static_cast<void>(::close(file));
        if (data_ == nullptr)
        {
            static_cast<void>(::shm_unlink(name_.c_str()));
            name_.clear();
        }
    }

    ~SharedBuffer()
    {
        release();
    }

    SharedBuffer(const SharedBuffer&) = delete;
    SharedBuffer& operator=(const SharedBuffer&) = delete;

    SharedBuffer(SharedBuffer&& other) noexcept
    {
        swap(other);
    }

    SharedBuffer& operator=(SharedBuffer&& other) noexcept
    {
        swap(other);
        return *this;
    }

    explicit operator bool() const noexcept
    {
        return data_ != nullptr;
    }

    [[nodiscard]] T* data() const noexcept
    {
        return data_;
    }

    [[nodiscard]] T& operator[](std::uint64_t index) const noexcept
    {
        return data_[index];
    }

    [[nodiscard]] std::uint64_t count() const noexcept
    {
        return count_;
    }

    // The size lives next to the pointer, so nothing has to work it out a second time to
    // give it back.
    [[nodiscard]] std::uint64_t bytes() const noexcept
    {
        return count_ * sizeof(T);
    }

    [[nodiscard]] const std::string& name() const noexcept
    {
        return name_;
    }

    // Start every element at a value no child would report, so "nothing written here" is
    // told apart from "written, and it failed".
    void fill(const T& value) const noexcept
    {
        for (std::uint64_t index = 0; index < count_; ++index)
        {
            data_[index] = value;
        }
    }

private:
    void release() noexcept
    {
        if (data_ != nullptr)
        {
            static_cast<void>(::munmap(data_, static_cast<std::size_t>(bytes())));
            data_ = nullptr;
        }
        // Unmapping touches this process's address space alone, so a forked child may do
        // it. The name is shared, so only the process that made it takes that away.
        if (!name_.empty() && ::getpid() == owner_)
        {
            static_cast<void>(::shm_unlink(name_.c_str()));
        }
        name_.clear();
    }

    void swap(SharedBuffer& other) noexcept
    {
        std::swap(data_, other.data_);
        std::swap(count_, other.count_);
        std::swap(owner_, other.owner_);
        name_.swap(other.name_);
    }

    T* data_{nullptr};
    std::uint64_t count_{0};
    std::string name_;
    ::pid_t owner_{0};
};

class TestContext
{
public:
    TestContext(int argc, char** argv)
        : argc_{opts_.parse(argc, argv)},
          argv_{argv},
          config_{opts_.configPath},
          memory_{TestMemory::open(config_, opts_.backend, opts_.caseName, opts_.slot)},
          strategy_{opts_.strategyChoice()}
    {
        // Constructing the memory already cleared the object, which is all --cleanup asks
        // for. ctest registers that as its own entry, so there is no run to make after it.
        if (opts_.cleanup)
        {
            throw CleanupRequested_t{};
        }
    }

    TestContext(const TestContext&) = delete;
    TestContext& operator=(const TestContext&) = delete;

    // ── the medium ─────────────────────────────────────────────────────

    // The area this run owns: its URI for the library, its raw bytes for a probe, a second
    // region view for a case that injects a fault.
    [[nodiscard]] TestMemory& memory() const noexcept
    {
        return *memory_;
    }

    [[nodiscard]] const std::string& uri() const
    {
        return uriCache_.empty() ? (uriCache_ = memory_->uri()) : uriCache_;
    }

    [[nodiscard]] cme::CoherencyMode coherency() const noexcept
    {
        return memory_->coherency();
    }

    [[nodiscard]] const char* backendName() const noexcept
    {
        return memory_->name().c_str();
    }

    [[nodiscard]] const ConfigReader& config() const noexcept
    {
        return config_;
    }

    // A buffer for this run's children to report through. @label tells one from another when
    // a case needs more than one, and the run's own name plus its pid keep two runs apart.
    template <typename T>
    [[nodiscard]] SharedBuffer<T> scratch(const std::string& label, std::uint64_t count) const
    {
        return SharedBuffer<T>{shmName(opts_.caseName, label), count};
    }

    // ── the policy ─────────────────────────────────────────────────────

    [[nodiscard]] cme::Strategy strategy() const noexcept
    {
        return strategy_.strategy;
    }

    [[nodiscard]] const char* strategySuffix() const noexcept
    {
        return strategy_.suffix;
    }

    // ── the verdict ────────────────────────────────────────────────────

    // Print a PASS/FAIL line and tally. Non-fatal: the run keeps going, so one run reports
    // every invariant it broke rather than only the first.
    // Returns @cond, so a case whose later steps depend on this one can write
    // `if (!ctx.check(...)) { return; }`. Most callers ignore it and keep going.
    //
    // Delegated, all four: what a probe records and what a case records are the same thing, and two
    // copies of it are two line shapes a reader has to compare to know they agree.
    bool check(bool cond, const char* msg)
    {
        return verdict_.check(cond, msg);
    }

    // check() for a message that has to carry a runtime value. Same verdict, same tally, same
    // line shape. The format attribute is what check() gets for free from a plain string: it
    // makes the compiler reject a %u handed a 64-bit value.
    [[gnu::format(printf, 3, 4)]] bool checkf(bool cond, const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        const bool answer = verdict_.checkv(cond, fmt, args);
        va_end(args);
        return answer;
    }

    // For a run that prints its own failure line and only needs the tally.
    void recordFailure() noexcept
    {
        verdict_.recordFailure();
    }

    // End the run as skipped. Asked before any check, since a case that reaches its assertions on
    // a build that compiled its subject out is asserting about nothing.
    [[noreturn]] static void skip(const char* reason)
    {
        throw SkipRequested_t{reason};
    }

    // Read by runCase alone. A run can add to the tally and cannot take anything off it.
    [[nodiscard]] std::int32_t failures() const noexcept
    {
        return verdict_.failures();
    }

    // ── what the harness did not take ──────────────────────────────────

    [[nodiscard]] int argc() const noexcept
    {
        return argc_;
    }

    [[nodiscard]] char** argv() const noexcept
    {
        return argv_;
    }

private:
    // Declaration order is the contract. parse() fills opts_, config_ reads the file it
    // names, the medium is built from both, and the policy is read off the flags last.
    Options_t opts_{};
    probe::Context verdict_;
    int argc_;
    char** argv_;
    ConfigReader config_;
    std::unique_ptr<TestMemory> memory_;
    StrategyChoice_t strategy_;

    // uri() hands out a reference, so the string has to outlive the call. Built once: the
    // medium cannot change under a run.
    mutable std::string uriCache_;
};

// The run in progress, for the helpers under harness/.
//
// One TestContext exists per process: runCase builds it, runs one body against it, and tears it
// down. Passing it into every helper made each signature carry the run rather than the request, so
// it is held here and the helpers ask for it.
//
// The order test_context.hpp exists to enforce is unaffected: the members still settle in
// declaration order, and the pointer below only names the finished object. What a global does
// reintroduce is the question of whether it is set, so reaching for a run that is not there aborts
// with a line saying so rather than dereferencing null three frames deeper.
inline TestContext*& currentRunSlot() noexcept
{
    static TestContext* slot = nullptr;
    return slot;
}

[[nodiscard]] inline TestContext& currentRun()
{
    TestContext* slot = currentRunSlot();
    if (slot == nullptr)
    {
        std::fprintf(stderr, "harness: no run in progress (a helper was called outside runCase)\n");
        std::abort();
    }
    return *slot;
}

// Publishes @ctx as the run for as long as it is in scope. A guard rather than two statements, so
// a body that throws still leaves nothing behind.
class RunScope
{
public:
    explicit RunScope(TestContext& ctx) noexcept
    {
        currentRunSlot() = &ctx;
    }
    RunScope(const RunScope&) = delete;
    RunScope& operator=(const RunScope&) = delete;
    ~RunScope()
    {
        currentRunSlot() = nullptr;
    }
};

// Runs one case and returns the code ctest reads. Deriving it here is the point: a run that
// prints PASS cannot also return non-zero, because it no longer writes either one. A skip
// is not a failure and a cleanup is not a run, so each leaves by its own door.
template <typename T_Body>
int runCase(int argc, char** argv, T_Body&& body)
{
    std::int32_t failures = 0;
    try
    {
        TestContext ctx{argc, argv};
        const RunScope scope{ctx};  // the helpers reach for it through currentRun()

        // Caught inside ctx's lifetime, so a body that threw still reports the checks it
        // did reach. Counted too: a run that died on the way has not passed, however few
        // checks it got through.
        try
        {
            body(ctx);
        }
        catch (const std::exception& e)
        {
            std::printf("FAIL: uncaught exception: %s\n", e.what());
            ctx.recordFailure();
        }
        failures = ctx.failures();
    }
    catch (const CleanupRequested_t&)
    {
        return 0;  // the file is gone; there was no run to make
    }
    catch (const SkipRequested_t& skipped)
    {
        std::printf("SKIP: %s\n", skipped.reason);
        return SkipExitCode;
    }
    catch (const MediumUnavailable& e)
    {
        std::printf("SKIP: %s\n", e.what());
        return SkipExitCode;
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: could not start the run: %s\n", e.what());
        failures = 1;
    }

    std::printf("\nRESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace harness
