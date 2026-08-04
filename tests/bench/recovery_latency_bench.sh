#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# recovery_latency_bench.sh -- recovery timeline per strategy, swept over the dead
# peer's owned-domain count.
#
# One run covers one strategy, because the phase boundaries are strategy-specific:
# how long detection takes, and how takeover is ordered, is exactly what the four
# differ in. Running the set in one invocation puts their columns side by side.
#
# BACKEND picks the medium. shm needs no device, so it is the default and runs
# anywhere; dax and uc need config.yaml to name one, and the binary exits with the
# skip code when it does not.
#
# A run on dax lands on the slot dax_slot_reserve holds back, not offset 0, so a
# filesystem sharing the device is not overwritten. SLOT picks which slot.
#
set -euo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

USAGE="usage: recovery_latency_bench.sh [--backend shm|dax|uc] [--strategies \"request order\"]
                                  [--peers N] [--domains N] [--slot N] [--build DIR]
  --domains a single count; omitted, the binary sweeps its own set"

BACKEND=shm
STRATEGIES="request order request-agg peterson"
PEERS=6
DOMAINS=""
script_parse_args "$USAGE" "backend strategies peers domains slot build" "" "$@"
script_require_choice --backend "$BACKEND" "shm dax uc"

THP="$(bench_thp)"
BIN="$(bench_bin cme-recovery-latency-bench)"
bench_require "$BIN" "$THP"

for strategy in $STRATEGIES; do
    echo "############ $strategy (backend=$BACKEND, peers=$PEERS) ############"
    if [ -n "${DOMAINS:-}" ]; then
        "$THP" "$BIN" --backend "$BACKEND" --strategy "$strategy" --slot "$SLOT" \
            --peers "$PEERS" --domains "$DOMAINS"
    else
        "$THP" "$BIN" --backend "$BACKEND" --strategy "$strategy" --slot "$SLOT" \
            --peers "$PEERS"
    fi
    echo
done
