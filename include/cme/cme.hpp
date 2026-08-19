// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cme.hpp -- umbrella include for the v0.2 public API.

#pragma once

// Exported, all three: re-exporting is the whole of what this file does, so a caller who includes it
// spells names none of these lines are used for here.
#include "cme/errors.hpp"          // IWYU pragma: export
#include "cme/shared.hpp"          // IWYU pragma: export
#include "cme/shared_session.hpp"  // IWYU pragma: export
