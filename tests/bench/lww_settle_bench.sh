#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# lww_settle_bench.sh -- settle-time sweep for the atomic-free LWW claim.
#
# Two phases over the same contender set:
#   UC   : a file under file_backend_dir, the regime ClaimSettle has to cover
#   heap : a plain DRAM line, the write-back floor the UC numbers are read against
# The UC phase is the one that sets the constant. The heap phase says how much of
# the UC cost is the medium rather than thread scheduling on this host.
#
# Read settle_p99 and settle_max. How closely the stakers arrive decides whether a
# round holds a genuine race or a near-miss, and that skew tracks code layout, so
# settle_p50 says which mix a given build sampled rather than what a race costs.
#
set -euo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

USAGE="usage: lww_settle_bench.sh [--phase both|uc|heap] [--contenders \"1 2 4\"]
                            [--repeats N] [--slot N] [--build DIR]"

CONTENDERS="1 2 4 8 16 32 64"
REPEATS=200
PHASE=both
script_parse_args "$USAGE" "phase contenders repeats slot build" "" "$@"
script_require_choice --phase "$PHASE" "both uc heap"

UC_DIR="$(site_get file_backend_dir)"
THP="$(bench_thp)"
BIN="$(bench_bin cme-lww-settle-bench)"
bench_require "$BIN" "$THP"

ran=0

if [ "$PHASE" = both ] || [ "$PHASE" = uc ]; then
    if site_is_mounted "$UC_DIR"; then
        echo "############ UC phase ($UC_DIR) ############"
        "$THP" "$BIN" --backend uc --contenders "$CONTENDERS" --repeats "$REPEATS" --slot "$SLOT"
        ran=$((ran + 1))
    else
        echo ">>> skipping UC phase: '${UC_DIR:-<file_backend_dir unset>}' is not a mount point"
    fi
fi

if [ "$PHASE" = both ] || [ "$PHASE" = heap ]; then
    [ "$ran" -gt 0 ] && echo
    echo "############ heap phase (DRAM write-back baseline) ############"
    "$BIN" --contenders "$CONTENDERS" --repeats "$REPEATS"
    ran=$((ran + 1))
fi

[ "$ran" -gt 0 ] || echo "nothing to measure" >&2
