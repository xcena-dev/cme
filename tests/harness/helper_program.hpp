// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_program.hpp -- running a built binary and reading what it left with.
//
// helper_process.hpp forks a body the case wrote. This execs a program the build produced, which is
// a different subject: what is under test is the exit code and the side effects of the shipped tool
// itself, not a body compiled into the case.
//
// Exit codes and not stderr. A supervisor and a deployment script read the code, so that is the
// contract worth holding a tool to.
//
// Links nothing. A case here reaches for POSIX process control and std only, so the cmed probes can
// include it without taking libcme through it.

#pragma once

#include <sys/wait.h>
#include <unistd.h>

// kill and SIGKILL reach us through <csignal> on a POSIX libc, but the standard promises only the
// six C signals, so include-cleaner cannot see them there and would send us to the deprecated
// signal.h.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace harness
{

// argv as the program will see it, NULL terminated. Built here so a caller passes strings and never
// a char* array it has to keep alive itself.
[[nodiscard]] inline std::vector<char*> asArgv(const std::vector<std::string>& words)
{
    std::vector<char*> argv;
    argv.reserve(words.size() + 1);
    for (const std::string& word : words)
    {
        argv.push_back(const_cast<char*>(word.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

// A program started and left running. The destructor kills rather than waits: a case that failed
// before it could stop one must not hang the suite waiting for a daemon that serves forever.
class RunningProgram
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    explicit RunningProgram(std::vector<std::string> words)
        : words_{std::move(words)},
          child_{::fork()}
    {
        if (child_ != 0)
        {
            return;
        }

        const std::vector<char*> argv = asArgv(words_);
        ::execv(argv[0], argv.data());

        // Only reached when execv failed, and the child must not run the case's own exit path.
        std::perror("execv");
        std::_Exit(127);
    }

    RunningProgram(const RunningProgram&) = delete;
    RunningProgram(RunningProgram&&) = delete;

    ~RunningProgram() noexcept
    {
        if (child_ > 0)
        {
            ::kill(child_, SIGKILL);  // NOLINT(misc-include-cleaner) POSIX, via <csignal>
            int status = 0;
            static_cast<void>(::waitpid(child_, &status, 0));
        }
    }

    // ── operator= ──────────────────────────────────────────────────
    RunningProgram& operator=(const RunningProgram&) = delete;
    RunningProgram& operator=(RunningProgram&&) = delete;

    // ── public methods ─────────────────────────────────────────────
    // Send @signalNumber and wait for the exit. Returns the code, or -1 if it left another way.
    // Once this returns the destructor has nothing to do.
    [[nodiscard]] int stopWith(int signalNumber) noexcept
    {
        if (child_ <= 0)
        {
            return -1;
        }

        ::kill(child_, signalNumber);

        int status = 0;
        const ::pid_t reaped =  // NOLINT(misc-include-cleaner) POSIX, via <unistd.h>
            ::waitpid(child_, &status, 0);
        child_ = -1;
        return (reaped > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    }

    // ── accessors ──────────────────────────────────────────────────
    // What the started program writes about itself, so a case can tell one instance's work from
    // another's.
    [[nodiscard]] ::pid_t pid() const noexcept  // NOLINT(misc-include-cleaner) POSIX, via <unistd.h>
    {
        return child_;
    }

private:
    std::vector<std::string> words_;
    ::pid_t child_;
};

// Run to completion and hand back the code it left with, or -1 if it left another way.
[[nodiscard]] inline int runProgram(const std::vector<std::string>& words)
{
    RunningProgram running{words};

    // Signal 0 delivers nothing, so this is only the wait. A program that has already exited is
    // reaped by it just the same.
    return running.stopWith(0);
}

}  // namespace harness
