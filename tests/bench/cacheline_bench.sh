#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# cacheline_bench.sh -- load/store ns/iter, uncacheable versus write-back, same media.
#   UC : a file under file_backend_dir, where an uncacheable mount makes stores
#        need no flush
#   WB : the raw devdax node, where stores need clwb/clflush+sfence
# Running both over one device is the point: it isolates the cache regime rather
# than the medium.
#
# Whether the mount is truly uncacheable is a property of how that filesystem was
# built and mounted, which this script neither sets nor inspects. A phase whose
# medium this machine lacks is skipped.
#
# The WB phase lands on the slot dax_slot_reserve holds back, not offset 0, so a
# filesystem sharing the device is not overwritten. --slot picks which slot.
set -euo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

USAGE="usage: cacheline_bench.sh [--phase both|uc|wb] [--iters N] [--slot N] [--build DIR]"

ITERS=200000
PHASE=both
script_parse_args "$USAGE" "phase iters slot build" "" "$@"
script_require_choice --phase "$PHASE" "both uc wb"

UC_DIR="$(site_get file_backend_dir)"
DEV="$(site_get dax_device)"
THP="$(bench_thp)"
BIN="$(bench_bin cme-cacheline-bench)"
bench_require "$BIN" "$THP"

ran=0

if [ "$PHASE" = both ] || [ "$PHASE" = uc ]; then
    if site_is_mounted "$UC_DIR"; then
        echo "############ UC phase ($UC_DIR) ############"
        UC_FILE="$UC_DIR/cl_bench"
        rm -f "$UC_FILE" 2>/dev/null || true
        "$THP" "$BIN" --backend uc --iters "$ITERS"
        rm -f "$UC_FILE" 2>/dev/null || true
        ran=$((ran + 1))
    else
        echo ">>> skipping UC phase: '${UC_DIR:-<file_backend_dir unset>}' is not a mount point"
    fi
fi

if [ "$PHASE" = both ] || [ "$PHASE" = wb ]; then
    if [ -c "$DEV" ]; then
        [ "$ran" -gt 0 ] && echo
        echo "############ WB phase (raw devdax $DEV, slot $SLOT) ############"
        "$THP" "$BIN" --backend dax --iters "$ITERS" --slot "$SLOT"
        ran=$((ran + 1))
    else
        echo ">>> skipping WB phase: '${DEV:-<dax_device unset>}' is not a character device"
    fi
fi

[ "$ran" -gt 0 ] || echo "nothing to measure" >&2
