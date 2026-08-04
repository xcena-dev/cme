#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# bench_sweep.sh -- one binary over the (peers x domains) grid, aggregated into a table
# and a CSV of the acquire-latency distribution (mean/p50/p90/p99).
#
# cme-fairness-test already takes -n/-d/-i and prints an "acquire latency (...)" line, so
# this only runs the grid and parses. run_sweep.sh calls it per cell; run it directly when
# you want one row rather than the whole table.
#
# To compare a baseline against a change, build each into its own tree and run this once
# per tree with --bin. To add a poll dimension, vary --poll-us and give each run its own
# --csv.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../harness/script_args.sh"

USAGE="usage: bench_sweep.sh [--bin PATH] [--repeat N] [--iters N] [--strategy --request]
                     [--csv PATH] [--poll-us N] [--hold-ms N]
                     [--backend shm|dax|uc] [--case NAME] [--shuffle]
  --case names the uc file the harness places under file_backend_dir, so two sweeps
         running at once need two names"

BIN=./tests/cme-fairness-test
REPEAT=10
ITERS=1000
STRATEGY=--request
CSV=/tmp/bench_sweep.csv
POLL_US=10        # poll-thread cadence, --poll-us
HOLD_MS=0         # critical-section hold, --cs-sleep
BACKEND=shm
CASE=bench_sweep
SHUFFLE=0         # randomise each thread's per-sweep domain order
script_parse_args "$USAGE" \
	"bin repeat iters strategy csv poll-us hold-ms backend case" "shuffle" "$@"
script_require_choice --backend "$BACKEND" "shm dax uc"

SHUF=""
[ "$SHUFFLE" = 1 ] && SHUF="--shuffle"
BACKEND_ARGS=(--backend "$BACKEND" --case "$CASE")

PEERS=(4 8 16 32 64)
DOMAINS=(1 4 16 32 64)

if [[ ! -x "$BIN" ]]; then
    echo "no executable: $BIN" >&2
    exit 1
fi

# Snapshot the binary so a concurrent rebuild of $BIN can't change what we
# measure mid-sweep. Run from the copy; delete it on any exit (normal/ctrl-c).
SNAP="$(mktemp /tmp/cme-fairness-snap.XXXXXX)"
cp "$BIN" "$SNAP"
chmod +x "$SNAP"
trap 'rm -f "$SNAP"' EXIT
RUN="$SNAP"

echo "bench: $BIN (snapshot $SNAP)  repeat=$REPEAT iters=$ITERS strategy=$STRATEGY poll=${POLL_US}us hold=${HOLD_MS}ms"
printf "%-5s %-4s %9s %9s %9s %9s\n" peers dom mean_us p50_us p90_us p99_us
echo "peers,domains,mean_us,p50_us,p90_us,p99_us,samples" > "$CSV"

for n in "${PEERS[@]}"; do
    for d in "${DOMAINS[@]}"; do
        tmp="$(mktemp)"
        for ((r = 0; r < REPEAT; r++)); do
            # uc backend on WORM: each run needs a fresh file, since a re-format is
            # rejected once committed. --cleanup removes it and exits; shm and dax
            # have nothing to remove.
            [[ "$BACKEND" == uc ]] && "$RUN" "${BACKEND_ARGS[@]}" --cleanup >/dev/null 2>&1
            "$RUN" "${BACKEND_ARGS[@]}" -n "$n" -d "$d" -i "$ITERS" "$STRATEGY" $SHUF \
                   --poll-us "$POLL_US" --cs-sleep "$HOLD_MS" 2>/dev/null \
                | grep "acquire latency" >> "$tmp"
        done
        # One line per run; the arithmetic mean of each percentile across them.
        stats="$(gawk '
            match($0, /mean=([0-9.]+)/, a) { m += a[1] }
            match($0, /p50=([0-9.]+)/,  b) { x += b[1] }
            match($0, /p90=([0-9.]+)/,  c) { y += c[1] }
            match($0, /p99=([0-9.]+)/,  e) { z += e[1]; k++ }
            END {
                if (k > 0) printf "%.2f %.2f %.2f %.2f %d", m/k, x/k, y/k, z/k, k
                else       printf "NA NA NA NA 0"
            }' "$tmp")"
        rm -f "$tmp"
        read -r me p50 p90 p99 k <<< "$stats"
        printf "%-5s %-4s %9s %9s %9s %9s\n" "$n" "$d" "$me" "$p50" "$p90" "$p99"
        echo "$n,$d,$me,$p50,$p90,$p99,$k" >> "$CSV"
    done
done

echo "CSV -> $CSV"
