#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# Sweep across successor strategies AND backends, side-by-side.
#   backend=shm  -> posix /dev/shm. WB-cached DRAM baseline; needs no hardware.
#   backend=file -> a file under file_backend_dir. Real UC mmap where that mount
#                   maps pgprot_noncached. The mount must already be up.
#   backend=dax  -> the devdax node named by dax_device, mapped directly.
# Runs each (backend x strategy) over the same (peers x domains) grid, prints one
# cross-strategy table per backend, then draws a comparison figure. A backend this
# machine cannot provide is dropped with a message. Build output hidden unless it fails.
set -euo pipefail

# ===== TUNABLE CONFIG =====
# What the contended sweep hands bench_sweep per cell: how many times to repeat the cell,
# and -i per (peer-thread, domain).
SWEEP_REPEAT=1; SWEEP_ITERS=100
TN=4; TD=1; TT=1; TI=300  # trace run (TRACE=1): cme-fairness-test -n/-d/-t/-i
FROM_US=500; WIN_US=25    # png trace window: latency_trace.py --from-us/--window-us
# seqlat trace run (TRACE=1 + SEQLAT=1): small sequential run -> handoff-stage swimlane.
# Window width from WIN_US (as sweep), but offset from t0 -- the short seqlat run
# finishes early, so a 500us offset would miss it. SEQ_TR_FROM=0 starts at trace t0.
SEQ_TR_N=4; SEQ_TR_D=2; SEQ_TR_I=30; SEQ_TR_W=15   # single domain (-d 2); short run
SEQ_TR_FROM=0; SEQ_TR_WIN=3000                     # seqlat png window (own, not WIN_US)
# peterson uses coherency::get/set (not the instrumented getF), so its LAT breakdown
# undercounts -- latency still valid.
STRATEGIES="peterson request request-agg order"
# Any subset of "shm file dax".
BACKENDS="shm file dax"
# The three benches are independently toggleable, so you can run just one, e.g.
#   ./run_sweep.sh --no-contended --no-seqlat --backends shm
CONTENDED=1
# Uncontended sequential acquire-latency bench: single driver, random (peer,domain), one
# lock in flight -> pure handoff cost. Swept over the same (peers x domains) grid as the
# contended sweep; plots the MIGRATE p50.
SEQLAT=1
SEQ_PEERS="4 8 16 32 64"   # peer counts
SEQ_DOMAINS="2 4 16 32 64" # -d values (>=2; slot 0 = control)
SEQ_ITERS=5000
SEQ_WARMUP=500
# Two-tier (PeerWorker) acquire-latency bench: peers x threads/peer on ONE shared domain
# -> intra-node mutex + inter-node CXL cost. CSV col 2 = threads/peer.
TIERLAT=1
# FAIR comparison: hold the TOTAL worker count fixed, vary only the peer/thread split
# (peers x threads = TIER_TOTAL). 64p x 1t (all inter-node CXL) ... 1p x 64t (all
# intra-node mutex) -- every row is the same total load, so the delta is pure tiering.
TIER_TOTAL=64
TIER_SPLIT_PEERS="4 8 16 32"  # peers; threads = TIER_TOTAL/peers
TIER_DOMAINS="1 4 16 32 63"   # data-domain counts (D+control <= MaxDomains=64; 63 max)
TIER_ITERS=2000               # iters per thread per split point
# Intra-node tier: mutex (PeerWorker cohort) | cme (per-node cme shm region; both tiers SWOT).
TIER_INNERS="mutex"
# Randomise per-sweep domain order.
SHUFFLE=0
# Also build + run the lat-breakdown/trace path, per backend and per strategy.
TRACE=0
BUILD=build
TBUILD=build-trace
PYTHON=python3   # --python ~/.venv/bin/python for matplotlib
# ==========================

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CME="$(cd "$HERE/../.." && pwd)"
. "$CME/tests/harness/script_args.sh"

USAGE="usage: run_sweep.sh [--backends \"shm file dax\"] [--strategies \"request order\"]
                    [--no-contended] [--no-seqlat] [--no-tierlat] [--trace] [--shuffle]
                    [--seq-peers \"4 8\"] [--seq-domains \"2 4\"] [--seq-iters N] [--seq-warmup N]
                    [--tier-total N] [--tier-split-peers \"4 8\"] [--tier-domains \"1 4\"]
                    [--tier-iters N] [--tier-inners \"mutex cme\"]
                    [--build DIR] [--tbuild DIR] [--python BIN]
  the three benches are independently toggleable; --trace adds the lat-breakdown path"

script_parse_args "$USAGE" \
	"backends strategies seq-peers seq-domains seq-iters seq-warmup
	 tier-total tier-split-peers tier-domains tier-iters tier-inners build tbuild
	 python" \
	"contended seqlat tierlat trace shuffle" "$@"
script_require_choices --backends "$BACKENDS" "shm file dax"
script_require_choices --strategies "$STRATEGIES" "peterson request request-agg order"
script_require_choices --tier-inners "$TIER_INNERS" "mutex cme"

# The trace runs need it on the command line; bench_sweep takes it the same way.
SHUF=""
[ "$SHUFFLE" = 1 ] && SHUF="--shuffle"

# Coherency is not set here: each binary derives it from --backend (see
# helper.hpp::famCoherency), and the uc mount maps pgprot_noncached.
. "$CME/tests/harness/site_config.sh"
UC_DIR="$(site_get file_backend_dir)"
DEV="$(site_get dax_device)"
TS=$(date +%H%M%S)

# Does BACKENDS contain a given backend?
has_backend() { case " $BACKENDS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

# BACKENDS names the medium; the binaries take --backend, where file means uc.
backend_flag() { case "$1" in file) echo uc ;; *) echo "$1" ;; esac; }

drop_backend() {
	local drop="$1" kept="" b
	for b in $BACKENDS; do [ "$b" = "$drop" ] || kept="$kept $b"; done
	BACKENDS="${kept# }"
}

quiet() {
	local log rc
	log=$(mktemp)
	if "$@" >"$log" 2>&1; then
		rm -f "$log"
	else
		rc=$?
		echo "FAILED ($rc): $*" >&2
		cat "$log" >&2
		rm -f "$log"
		exit "$rc"
	fi
}

csv_for() { echo "/tmp/sweep_${1}_${2}.csv"; }         # backend, strategy (contended)
seqlat_csv_for() { echo "/tmp/seqlat_${1}_${2}.csv"; }  # backend, strategy (sequential)
tiered_csv_for() { echo "/tmp/tiered_${1}_${2}_${3}.csv"; }  # backend, strategy, inner(mutex|cme)
tiered_csv_cur() { tiered_csv_for "$1" "$2" "$TIER_CUR_INNER"; }  # draw_group shim (2-arg)

# Cross-strategy mean/p99 table. $1=title, remaining args = one CSV per strategy
# (in $STRATEGIES order). Shared by the contended and sequential sweeps.
cross_table() {
	local title="$1"; shift
	echo
	echo "=== $title ==="
	"$PYTHON" - "$STRATEGIES" "$@" <<'PYEOF'
import csv, sys
strategies = sys.argv[1].split()
paths = sys.argv[2:]
data = {}
for strat, path in zip(strategies, paths):
    d = {}
    try:
        with open(path) as f:
            for r in csv.DictReader(f):
                # A point the driver could not measure carries NA; skip it so the cell prints "-".
                try:
                    point = (float(r['mean_us']), float(r['p99_us']))
                except ValueError:
                    continue
                d[(int(r['peers']), int(r['domains']))] = point
    except FileNotFoundError:
        pass
    data[strat] = d
keys = sorted(set().union(*[set(d) for d in data.values()]) or set())
W = 17
def cell(t):
    return "%-*s" % (W, "  -" if not t else "%-7.1f %-7.1f" % t)
hdr = "%-5s %-4s | " % ("peers", "dom") + "| ".join("%-*s" % (W, s) for s in strategies) + "| winner"
print(hdr)
print("-" * len(hdr))
last_n = None
for (n, d) in keys:
    if last_n is not None and n != last_n:
        print()
    last_n = n
    cells = [cell(data[s].get((n, d))) for s in strategies]
    ranked = sorted(((data[s].get((n, d))[0], s) for s in strategies if data[s].get((n, d))))
    win = "-"
    if ranked:
        best, ws = ranked[0]
        nxt = ranked[1][0] if len(ranked) > 1 else None
        margin = "" if nxt is None or best == 0 else "  (%.0f%%)" % (100.0 * (nxt - best) / best)
        win = "%s%s" % (ws, margin)
    print("%-5d %-4d | " % (n, d) + "| ".join(cells) + "| " + win)
PYEOF
}

# Tiered table: rows keyed (peers, threads, domains); columns = strategies; cell = mean/p99.
# $1=title, remaining args = one 8-col CSV (peers,threads,domains,...) per strategy.
tiered_table() {
	local title="$1"; shift
	echo
	echo "=== $title ==="
	"$PYTHON" - "$STRATEGIES" "$@" <<'PYEOF'
import csv, sys
strategies = sys.argv[1].split()
paths = sys.argv[2:]
data = {}
for strat, path in zip(strategies, paths):
    d = {}
    try:
        with open(path) as f:
            for r in csv.DictReader(f):
                # A point the driver could not measure carries NA; skip it so the cell prints "-".
                try:
                    point = (float(r['mean_us']), float(r['p99_us']))
                except ValueError:
                    continue
                d[(int(r['peers']), int(r['threads']), int(r['domains']))] = point
    except FileNotFoundError:
        pass
    data[strat] = d
keys = sorted(set().union(*[set(d) for d in data.values()]) or set())
W = 17
def cell(t):
    return "%-*s" % (W, "  -" if not t else "%-7.1f %-7.1f" % t)
hdr = "%-5s %-4s %-4s | " % ("peers", "thr", "dom") + "| ".join("%-*s" % (W, s) for s in strategies) + "| winner"
print(hdr); print("-" * len(hdr))
for (p, thr, dom) in keys:
    cells = [cell(data[s].get((p, thr, dom))) for s in strategies]
    ranked = sorted(((data[s].get((p, thr, dom))[0], s) for s in strategies if data[s].get((p, thr, dom))))
    win = "-"
    if ranked:
        best, ws = ranked[0]
        nxt = ranked[1][0] if len(ranked) > 1 else None
        margin = "" if nxt is None or best == 0 else "  (%.0f%%)" % (100.0 * (nxt - best) / best)
        win = "%s%s" % (ws, margin)
    print("%-5d %-4d %-4d | " % (p, thr, dom) + "| ".join(cells) + "| " + win)
PYEOF
}

# 1. Drop any backend this machine cannot provide, and sweep whatever is left.
#    An unmounted file_backend_dir is ordinary root-filesystem storage, so sweeping
#    it would measure WB DRAM and label the result UC.
if has_backend file && ! site_is_mounted "$UC_DIR"; then
	echo ">>> skipping backend=file: '${UC_DIR:-<file_backend_dir unset>}' is not a mount point"
	drop_backend file
fi
if has_backend dax && [ ! -c "$DEV" ]; then
	echo ">>> skipping backend=dax: '${DEV:-<dax_device unset>}' is not a character device"
	drop_backend dax
fi
if [ -z "$BACKENDS" ]; then
	echo "no backend left to sweep" >&2
	exit 0
fi

# 2. Build cme: sweep build (+ trace build if TRACE=1)
cd "$CME"
quiet cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
	-DME_STATS=OFF -DME_PROFILE=OFF -DME_LOGGING=OFF
quiet cmake --build "$BUILD" -j128
if [ "$TRACE" = 1 ]; then
	quiet cmake -S . -B "$TBUILD" -DCMAKE_BUILD_TYPE=Release \
		-DME_LATENCY=ON -DME_STATS=OFF -DME_PROFILE=OFF -DME_LOGGING=OFF
	quiet cmake --build "$TBUILD" -j128
fi

# 3. Per-backend, per-strategy contended sweep + optional trace. Only file needs a
#    distinct --case, which gives each sweep its own file under file_backend_dir.
if [ "$CONTENDED" = 1 ]; then
for B in $BACKENDS; do
	for S in $STRATEGIES; do
		CSV="$(csv_for "$B" "$S")"
		echo ">>> sweep backend=$B strategy=$S -> $CSV"
		cd "$CME/$BUILD"
		bash "$HERE/bench_sweep.sh" --backend "$(backend_flag "$B")" --case "sweep_${B}_$S" \
			--bin ./tests/cme-fairness-test --repeat "$SWEEP_REPEAT" --iters "$SWEEP_ITERS" \
			--strategy --"$S" --csv "$CSV" >/dev/null

		if [ "$TRACE" = 1 ]; then
			if [ "$S" = peterson ] && [ "$TT" -gt 1 ]; then
				echo "    (skip trace: peterson needs TT=1, got TT=$TT)"
			else
				png="/tmp/trace_${B}_${S}_$TS.png"
				jsonl="/tmp/trace_${B}_$S.jsonl"
				echo "    trace -> $jsonl, $png"
				cd "$CME/$TBUILD"
				./tests/cme-fairness-test --backend "$(backend_flag "$B")" --case "trace_${B}_$S" \
					-n "$TN" -d "$TD" -t "$TT" -i "$TI" --"$S" $SHUF --trace-jsonl "$jsonl" >/dev/null
				"$PYTHON" "$HERE/latency_trace.py" "$jsonl" --from-us "$FROM_US" --window-us "$WIN_US" -o "$png" >/dev/null
			fi
		fi
	done
done

fi

# 3b. Uncontended sequential acquire-latency SWEEP over the (peers x domains) grid,
#     per (backend x strategy) -> seqlat CSVs. The bench appends the MIGRATE stats
#     per point.
if [ "$SEQLAT" = 1 ]; then
	cd "$CME/$BUILD"
	for B in $BACKENDS; do
		for S in $STRATEGIES; do
			CSV="$(seqlat_csv_for "$B" "$S")"
			echo ">>> seqlat backend=$B strategy=$S -> $CSV"
			echo "peers,domains,mean_us,p50_us,p90_us,p99_us,samples" > "$CSV"
			for n in $SEQ_PEERS; do
				for d in $SEQ_DOMAINS; do
					if [ "$B" = file ]; then
						./tests/cme-seq-latency-test --backend uc --case "seqlat_${S}_${n}_${d}" \
							--"$S" -n "$n" -d "$d" \
								-i "$SEQ_ITERS" --warmup "$SEQ_WARMUP" --csv "$CSV" >/dev/null
					else
						./tests/cme-seq-latency-test --backend "$B" --"$S" -n "$n" -d "$d" \
							-i "$SEQ_ITERS" --warmup "$SEQ_WARMUP" --csv "$CSV" >/dev/null
					fi
				done
			done
		done
	done
fi

# 3c. seqlat handoff-stage swimlane trace (TRACE=1 + SEQLAT=1). Small sequential run
#     -> per-stage span timeline; the ~50us uncontended handoff shows up as
#     Spin/SpinPoll bars.
if [ "$SEQLAT" = 1 ] && [ "$TRACE" = 1 ]; then
	cd "$CME/$TBUILD"
	for B in $BACKENDS; do
		for S in $STRATEGIES; do
			png="/tmp/seqlat_trace_${B}_${S}_$TS.png"
			jsonl="/tmp/seqlat_trace_${B}_$S.jsonl"
			echo "    seqlat trace backend=$B strategy=$S -> $jsonl, $png"
			./tests/cme-seq-latency-test --backend "$(backend_flag "$B")" --case "seqlat_trace_${B}_$S" \
				--"$S" -n "$SEQ_TR_N" -d "$SEQ_TR_D" \
					-i "$SEQ_TR_I" --warmup "$SEQ_TR_W" --trace-jsonl "$jsonl" >/dev/null
			"$PYTHON" "$HERE/latency_trace.py" "$jsonl" --from-us "$SEQ_TR_FROM" --window-us "$SEQ_TR_WIN" -o "$png" >/dev/null
		done
	done
fi

# 3d. Two-tier (PeerWorker) acquire-latency SWEEP at FIXED total worker count, over a
#     (peer/thread split x domains) grid -> tiered CSVs. Total = peers x threads is held
#     constant per split (fair tiering comparison); each thread sweeps D domains (like #1).
if [ "$TIERLAT" = 1 ]; then
	cd "$CME/$BUILD"
	for B in $BACKENDS; do
		for INNER in $TIER_INNERS; do
			for S in $STRATEGIES; do
				CSV="$(tiered_csv_for "$B" "$S" "$INNER")"
				echo ">>> tiered backend=$B inner=$INNER strategy=$S (total=$TIER_TOTAL, domains=[$TIER_DOMAINS]) -> $CSV"
				echo "peers,threads,domains,mean_us,p50_us,p90_us,p99_us,samples" > "$CSV"
				for n in $TIER_SPLIT_PEERS; do
					t=$(( TIER_TOTAL / n ))
					# Only exact splits (peers*threads == total) so every point has equal load.
					[ "$(( n * t ))" -eq "$TIER_TOTAL" ] && [ "$t" -ge 1 ] || continue
					for d in $TIER_DOMAINS; do
						# $INNER only names the CSV and the log: cme-tiered-lock-test has no
						# inner-tier knob, so this axis currently varies nothing in the run.
						args=(--strategy "$S" --peers "$n" --threads "$t" --domains "$d"
						      --iters "$TIER_ITERS" --shuffle "$SHUFFLE" --csv "$CSV")
						if [ "$B" = file ]; then
							args=(--backend uc --case "tiered_$$_${S}_${INNER}_${n}_${t}_${d}" "${args[@]}")
						else
							args=(--backend "$B" "${args[@]}")
						fi
						# `if !` keeps a failing point (lock-deadline timeout for request/agg at
						# high peer count) from aborting the sweep under set -e; log it, carry on.
						flog="/tmp/tiered_fail_${B}_${S}_${INNER}_${n}_${t}_${d}.log"
						if ! ./tests/cme-tiered-lock-test "${args[@]}" >/dev/null 2>"$flog"; then
							echo "    (tiered $S inner=$INNER ${n}p x ${t}t ${d}dom FAILED -- see $flog)"
						else
							rm -f "$flog"
						fi
					done
				done
			done
		done
	done
fi

# 4. Cross-strategy comparison tables, one per backend (contended + sequential).
#    CSV cols: peers,domains,mean_us,p50,p90,p99,samples.
for B in $BACKENDS; do
	cross_table "backend=$B contended -- mean_us / p99_us per (peers x domains)" \
		$(for S in $STRATEGIES; do csv_for "$B" "$S"; done)
done
if [ "$SEQLAT" = 1 ]; then
	for B in $BACKENDS; do
		cross_table "backend=$B sequential(migrate) -- mean_us / p99_us per (peers x domains)" \
			$(for S in $STRATEGIES; do seqlat_csv_for "$B" "$S"; done)
	done
fi
if [ "$TIERLAT" = 1 ]; then
	for B in $BACKENDS; do
		for INNER in $TIER_INNERS; do
			tiered_table "backend=$B tiered inner=$INNER (total=$TIER_TOTAL, domains=[$TIER_DOMAINS]) -- mean_us / p99_us" \
				$(for S in $STRATEGIES; do tiered_csv_for "$B" "$S" "$INNER"; done)
		done
	done
fi

# 5. Draw figures: one combined (all backends) + one per backend.
#    plot_figure <out.png> <title> <label=csv>...  -> echoes out.png on success.
#    Two axis orientations per grouping: x=peers (facet per domain) and
#    x=domains (facet per peer). {all + each backend} x {peers,domains}.
FIGS=()
plot_figure() {
	local out="$1" title="$2" xmode="$3"; shift 3
	[ "$#" -gt 0 ] || return 0
	# Suppress the plot's inline path print -- it's reported in the summary below.
	if "$PYTHON" "$HERE/sweep_plot.py" "$@" --x "$xmode" -o "$out" --title "$title" >/dev/null; then
		FIGS+=("$out")
	fi
}

# draw_group <csv_fn> <file-prefix> <title-tag>: combined + per-backend figures,
# both axis orientations. Reused for the contended sweep and the sequential sweep.
draw_group() {
	local csvfn="$1" prefix="$2" tag="$3" X B S f
	for X in peers domains; do
		local COMBINED_ARGS=()
		for B in $BACKENDS; do
			for S in $STRATEGIES; do
				f="$("$csvfn" "$B" "$S")"
				[ -e "$f" ] && COMBINED_ARGS+=("$B/$S=$f")
			done
		done
		[ "${#COMBINED_ARGS[@]}" -gt 1 ] && plot_figure "/tmp/${prefix}_all_${X}_$TS.png" \
			"CME $tag -- backends=$BACKENDS strategies=$STRATEGIES (x=$X)" "$X" "${COMBINED_ARGS[@]}"

		for B in $BACKENDS; do
			local ARGS=()
			for S in $STRATEGIES; do
				f="$("$csvfn" "$B" "$S")"
				[ -e "$f" ] && ARGS+=("$B/$S=$f")
			done
			[ "${#ARGS[@]}" -gt 0 ] && plot_figure "/tmp/${prefix}_${B}_${X}_$TS.png" \
				"CME $tag -- backend=$B strategies=$STRATEGIES (x=$X)" "$X" "${ARGS[@]}"
		done
	done
}

draw_group csv_for sweep "contended-sweep"
[ "$SEQLAT" = 1 ] && draw_group seqlat_csv_for seqlat "seqlat-migrate"
if [ "$TIERLAT" = 1 ]; then
	for INNER in $TIER_INNERS; do
		TIER_CUR_INNER=$INNER
		draw_group tiered_csv_cur "tiered_$INNER" "tiered inner=$INNER (dom sweep)"
	done
fi

echo
echo "=== CSVs ==="
for B in $BACKENDS; do
	for S in $STRATEGIES; do
		f="$(csv_for "$B" "$S")"
		if [ -e "$f" ]; then printf '  %s  (%s)\n' "$f" "$(du -h "$f" | cut -f1)"; else printf '  %s  (MISSING)\n' "$f"; fi
		if [ "$SEQLAT" = 1 ]; then
			g="$(seqlat_csv_for "$B" "$S")"
			[ -e "$g" ] && printf '  %s  (%s)\n' "$g" "$(du -h "$g" | cut -f1)"
		fi
		if [ "$TIERLAT" = 1 ]; then
			for INNER in $TIER_INNERS; do
				h="$(tiered_csv_for "$B" "$S" "$INNER")"
				[ -e "$h" ] && printf '  %s  (%s)\n' "$h" "$(du -h "$h" | cut -f1)"
			done
		fi
	done
done

echo
echo "=== FIGURES ==="
if [ "${#FIGS[@]}" -gt 0 ]; then
	for f in "${FIGS[@]}"; do
		[ -e "$f" ] && printf '  %s  (%s)\n' "$f" "$(du -h "$f" | cut -f1)"
	done
else
	echo "  (none)"
fi
