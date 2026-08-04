// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// thp_exec.cpp -- clear PR_SET_THP_DISABLE, then exec a command.
//
// Some interactive harnesses start with THP disabled and propagate it to every child. That
// breaks devdax mmap on PMD-aligned chardevs -- the fault path cannot install a huge mapping
// and the first store takes SIGBUS. Anon/file mmap is unaffected, so it goes unnoticed until
// DAX work hits it. Needed only when the calling shell inherits that state.
//
// It lives with the harness rather than with the tools because its callers are here and in
// bench/: every ctest case on a FAM medium runs through it, and so does every benchmark
// script. Nothing in tools/ uses it.
//
// Build:  CMake target `thp_exec` (see tests/CMakeLists.txt).
// Usage:  build/tests/thp_exec <cmd> [args...]

#include <sys/prctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace
{

constexpr int UsageExitCode = 2;
constexpr int ExecFailedExitCode = 127;

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s CMD [ARGS...]\n", argv[0]);
        return UsageExitCode;
    }

    if (::prctl(PR_SET_THP_DISABLE, 0, 0, 0, 0) < 0)
    {
        std::perror("prctl(PR_SET_THP_DISABLE, 0)");
        // not fatal: maybe THP already enabled
    }

    ::execvp(argv[1], &argv[1]);
    std::fprintf(stderr, "exec %s: %s\n", argv[1], std::strerror(errno));
    return ExecFailedExitCode;
}
