// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper.hpp -- the sub-headers together, so a case includes one name and gets all of
// namespace harness.
//
// Split by what a piece depends on rather than by what it is about, because the dependency is what
// a reader has to know before using it:
//
//   helper_util.hpp      randomness, log lines, waiting on a predicate, catching one exception
//                        type, one summary statistic. No cme header, no TestContext.
//   helper_process.hpp   forking and reaping child processes. POSIX only.
//   helper_cme.hpp       the library calls every case makes the same way: format, open, and asking
//                        what domains a region holds. Needs cme and the TestContext.
//   helper_peer.hpp      one peer per thread, acquiring in a loop and freezable from outside, which
//                        is what a recovery case needs before it can crash anything.
//   helper_failpoint.hpp killing one forked child at a named boundary. Needs both POSIX and cme,
//                        which is why it is not in either of the two above.
//
// Header-only throughout: each test is its own binary, so the inline functions resolve to one
// instance per executable and there is no helper.cpp to link.

// The pragmas are what make this an umbrella rather than three unused includes: include-cleaner
// treats an exported include as if the includer declared the symbols itself, so a case that
// includes only this file is not told to name the sub-header each function came from.

#pragma once

// IWYU pragma: begin_exports
#include "helper_cme.hpp"
#include "helper_failpoint.hpp"
#include "helper_peer.hpp"
#include "helper_process.hpp"
#include "helper_util.hpp"
// IWYU pragma: end_exports
