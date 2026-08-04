#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# read_tail_bench.sh -- the four read-tail modes in order, over one target.
#
# Each mode removes all but one candidate cause of the tail, so the four runs
# together say which cause a tail belongs to. Reading one mode alone says nothing:
# read-only is the floor, and each later mode adds exactly one contender.
#
#   read-only    nobody writes; raw media plus interconnect jitter
#   write-read   this thread stores then loads; dirty-line turnaround
#   concurrent   a second thread stores while this one loads; write-read overlap
#   flush-read   concurrent, plus a clflushopt per load, so every read reaches media
#
# POLLERS adds untimed spin-readers to approximate waiter pile-up. STRIDE 0 puts
# them on the timed line; a non-zero stride gives each its own, which separates
# same-line contention from "K more reads in flight".
#
# The run lands on the slot dax_slot_reserve holds back, not offset 0, so a
# filesystem sharing the device is not overwritten. --slot picks which slot.
set -euo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

USAGE="usage: read_tail_bench.sh [--target PATH] [--iters N] [--pollers N] [--stride BYTES]
                           [--slot N] [--build DIR]
  --target defaults to dax_device from config.yaml
  --stride 0 puts every poller on the timed line; a non-zero stride gives each its own"

ITERS=2000000
POLLERS=0
STRIDE=0
TARGET=""
script_parse_args "$USAGE" "target iters pollers stride slot build" "" "$@"

: "${TARGET:=$(site_get dax_device)}"
THP="$(bench_thp)"
BIN="$(bench_bin cme-read-tail-bench)"
bench_require "$BIN" "$THP"

if [ -z "$TARGET" ]; then
    echo "$0: no --target and no dax_device in config.yaml" >&2
    exit 2
fi
if [ ! -e "$TARGET" ]; then
    echo "$0: '$TARGET' does not exist" >&2
    exit 2
fi

echo "# read tail  target=$TARGET iters=$ITERS pollers=$POLLERS stride=$STRIDE slot=$SLOT"
for mode in read-only write-read concurrent flush-read; do
    "$THP" "$BIN" --target "$TARGET" --mode "$mode" --iters "$ITERS" \
        --pollers "$POLLERS" --stride "$STRIDE" --slot "$SLOT"
done
