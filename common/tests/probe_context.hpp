// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// probe_context.hpp -- what one probe records, and the entry point that runs it.
//
// A probe that returns a bare bool says a run failed and not which expectation failed, so the
// reader's next move is to add printf and run it again. Each check names itself as it goes, and the
// exit code is derived from the tally: the verdict and the code have one source, so they cannot
// disagree.
//
// Here rather than with either library's tests because both use it: cmed's probes and the probes
// over this directory's own headers. It reaches for neither, which is what lets it sit between them.

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <exception>

namespace probe
{

class Context
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    Context() noexcept = default;

    Context(const Context&) = delete;
    Context(Context&&) = delete;
    ~Context() noexcept = default;

    // ── operator= ──────────────────────────────────────────────────
    Context& operator=(const Context&) = delete;
    Context& operator=(Context&&) = delete;

    // ── public methods ─────────────────────────────────────────────
    // Records one expectation and returns what it was given, so a case can both report and branch
    // on the same call.
    bool check(bool held, const char* what)
    {
        std::printf("  %s : %s\n", held ? "OK  " : "FAIL", what);
        std::fflush(stdout);
        if (!held)
        {
            ++failures_;
        }
        return held;
    }

    // check() for a message carrying a runtime value. Same verdict, same tally, same line shape.
    // The format attribute is what check() gets for free from a plain string: it makes the compiler
    // reject a %u handed a 64-bit value.
    [[gnu::format(printf, 3, 4)]] bool checkf(bool held, const char* format, ...)
    {
        std::printf("  %s : ", held ? "OK  " : "FAIL");
        std::va_list args;
        va_start(args, format);
        // The va_list checker loses the va_start above when this header is analysed through another one.
        // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
        std::vprintf(format, args);
        va_end(args);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        if (!held)
        {
            ++failures_;
        }
        return held;
    }

    // Names the group the checks below it belong to. A probe covers several cases and the reader
    // has to know which one a FAIL line sits under.
    void openCase(const char* what)
    {
        std::printf("%s\n", what);
        std::fflush(stdout);
    }

    // For a failure with no expectation behind it, such as a body that threw.
    void recordFailure() noexcept
    {
        ++failures_;
    }

    // ── accessors ──────────────────────────────────────────────────
    // Read by run() alone. A probe can add to the tally and cannot take anything off it.
    [[nodiscard]] std::int32_t failures() const noexcept
    {
        return failures_;
    }

private:
    std::int32_t failures_{0};
};

// Runs one probe and returns the code ctest reads. The catch sits inside the tally so a body that
// threw still reports the checks it reached, and the throw itself counts as a failure.
template <typename T_Body>
int run(const char* name, T_Body&& body)
{
    Context ctx;
    std::printf("%s\n", name);

    try
    {
        body(ctx);
    }
    catch (const std::exception& failure)
    {
        std::printf("  FAIL : uncaught exception: %s\n", failure.what());
        ctx.recordFailure();
    }

    const std::int32_t failures = ctx.failures();
    std::printf("RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace probe
