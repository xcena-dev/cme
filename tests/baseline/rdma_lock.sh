#!/usr/bin/env bash
# RDMA remote-lock sweep -- the hardware-atomic reference point for CME's acquire
# numbers. A remote lock is a single 8-byte word acquired by the NIC, so the server
# CPU is not in the lock path at all. That is what makes it the fair opponent.
#
# Two hosts on the same RoCEv2 fabric, both naming the same HCA and GID index. This
# host is the client and runs the contending threads and QPs. The peer holds the lock
# words and otherwise idles. Both come from config.yaml (sweep_peer_host,
# roce_peer_ip, roce_hca, roce_gid_index, sweep_peer_dir).
#
# Needs, beyond config.yaml: passwordless ssh to the peer, a configured build tree here
# (the CMake target `rdma-lock-baseline`, whose binary is scp'd to the peer so both ends
# run the same bytes), and perftest's ib_atomic_lat on both hosts unless --no-baseline.
#
# Per (peers x domains) cell: peers RC QPs each acquire / trivial-CS / release,
# sweeping D page-spaced (4 KiB) independent lock words in a fresh random permutation
# per sweep, which matches the fairness sweep's shuffled access pattern. Emits a CSV row
# per cell, a cross-table in run_sweep's shape, and sweep_plot.py figures.
#
# Reduced smoke run (proves end-to-end without the multi-minute 64-peer cells):
#   ./tests/baseline/rdma_lock.sh --peers "4 8" --domains "1 4" --iters 200
# Full 5x5 grid: run it with no overrides.
# Correctness soak instead of latency:  ./tests/baseline/rdma_lock.sh --verify
# A different acquire policy:           ./tests/baseline/rdma_lock.sh --acquire backoff
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CME="$(cd "$HERE/../.." && pwd)"
. "$HERE/../harness/script_args.sh"
. "$HERE/../harness/site_config.sh"

USAGE="usage: rdma_lock.sh [--peers \"4 8\"] [--domains \"1 4\"] [--iters N] [--warmup N]
                   [--acquire ticket|backoff|blind] [--verify] [--no-baseline]
                   [--verify-cells \"8:4 16:1\"] [--verify-iters N] [--build DIR]
                   [--server-host H] [--server-roce-ip IP] [--hca NAME]
                   [--gid-index N] [--remote-dir DIR] [--python BIN]
  --verify        mutual-exclusion soak on a few cells instead of the latency sweep
  --no-baseline   skip the uncontended ib_atomic_lat step
  the site options default to config.yaml; pass them to run without one"

BUILD=build
PEERS="4 8 16 32 64"        # contender / QP counts (client threads)
DOMAINS="1 4 16 32 64"      # independent lock words (page-spaced 4 KiB)
ITERS=1000                  # measured sweeps per thread (samples = iters*D)
WARMUP=100                  # warmup sweeps per thread (unmeasured)
BASELINE=1                  # Step 0: ib_atomic_lat uncontended baseline

# Acquire policy. Default ticket, because that is what the design record measures CME
# against. Blind is the O(N^2) lock storm; no lock manager ships it, so it is a worst case
# to exhibit, not an opponent.
ACQUIRE=ticket

# Correctness soak, off by default: a mutual-exclusion check on a few representative cells
# (software counter RMW under the lock) printing PASS/FAIL, instead of the latency sweep.
# Cells are "peers:domains" tokens.
VERIFY=0
VERIFY_CELLS="8:4 16:1"
VERIFY_ITERS=500

# Empty until the parse runs, then filled from config.yaml. Reading the file first would
# make site_require exit on a machine that has no config.yaml even when every value was
# passed on the command line.
SERVER_HOST=""
SERVER_ROCE_IP=""
HCA=""
GID_INDEX=""
REMOTE_DIR=""
PYTHON=""

script_parse_args "$USAGE" \
	"peers domains iters warmup acquire verify-cells verify-iters build
	 server-host server-roce-ip hca gid-index remote-dir python" \
	"verify baseline" "$@"
script_require_choice --acquire "$ACQUIRE" "ticket backoff blind"
ACQ_FLAG="--$ACQUIRE"

: "${SERVER_HOST:=$(site_require sweep_peer_host)}"      # ssh target, management net
: "${SERVER_ROCE_IP:=$(site_require roce_peer_ip)}"
: "${HCA:=$(site_require roce_hca)}"                    # same HCA name on both hosts
: "${REMOTE_DIR:=$(site_require sweep_peer_dir)}"       # scratch dir, relative to remote $HOME
: "${GID_INDEX:=$(site_get roce_gid_index)}"            # RoCEv2 IPv4 GID index
: "${PYTHON:=$(site_get sweep_python)}"
: "${GID_INDEX:=3}"
: "${PYTHON:=python3}"

BIN="$CME/$BUILD/tests/baseline/rdma-lock-baseline"  # client side, built by CMake
RBIN="$REMOTE_DIR/rdma_lock"          # server-side path, expanded on the remote host
# Both ends must name the same card and GID, and rdma_lock picks the host's first HCA
# when it is given neither.
DEV_ARGS=(--dev "$HCA" --gid "$GID_INDEX")
TS=$(date +%H%M%S)
SSH="ssh -o BatchMode=yes"

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

csv_path() { echo "/tmp/rdma_sweep_${1}.csv"; }   # $1 = backend tag, e.g. rdma_ticket

# Kill any rdma_lock server on the peer (pkill by exact name; never leaves orphans).
kill_remote_server() { $SSH "$SERVER_HOST" 'pkill -x rdma_lock 2>/dev/null; exit 0'; }

cleanup() { kill_remote_server >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM

# ---- Deploy the built binary to the peer. Both ends must run the same bytes. ----
if [ ! -x "$BIN" ]; then
	echo "missing $BIN -- build first: cmake -S $CME -B $CME/$BUILD && cmake --build $CME/$BUILD" >&2
	echo "  (no target means libibverbs was not found when the tree was configured)" >&2
	exit 2
fi
echo ">>> scp rdma-lock-baseline to $SERVER_HOST (server)"
quiet $SSH "$SERVER_HOST" "mkdir -p $REMOTE_DIR"
quiet scp -o BatchMode=yes "$BIN" "$SERVER_HOST:$RBIN"

# ---- Step 0: uncontended single-CAS baseline via stock ib_atomic_lat. ----
BASE_US="n/a"
if [ "$BASELINE" = 1 ]; then
	echo ">>> Step 0: ib_atomic_lat uncontended single-CAS baseline (peer serves, this host drives)"
	kill_remote_server >/dev/null 2>&1 || true
	$SSH "$SERVER_HOST" 'pkill -x ib_atomic_lat 2>/dev/null; exit 0'
	$SSH "$SERVER_HOST" "nohup ib_atomic_lat -d $HCA -x $GID_INDEX -A CMP_AND_SWAP -F --perform_warm_up > $REMOTE_DIR/atomic_lat.log 2>&1 < /dev/null & exit 0"
	sleep 1
	alog=$(mktemp)
	if ib_atomic_lat -d "$HCA" -x "$GID_INDEX" -A CMP_AND_SWAP -F --perform_warm_up "$SERVER_ROCE_IP" >"$alog" 2>&1; then
		# perftest table row: bytes iters t_min t_max t_typical t_avg ...
		BASE_US=$(awk '/^ *8 /{print $5; exit}' "$alog")
		[ -n "$BASE_US" ] && echo "    uncontended single-CAS t_typical = ${BASE_US} us" \
			|| { echo "    (could not parse ib_atomic_lat output)"; cat "$alog"; }
	else
		echo "    (ib_atomic_lat baseline FAILED -- carrying on)"; cat "$alog" >&2
	fi
	rm -f "$alog"
	$SSH "$SERVER_HOST" 'pkill -x ib_atomic_lat 2>/dev/null; exit 0'
fi

# start a per-cell server on the peer and wait until it is listening. $1=peers $2=domains.
start_cell_server() {
	kill_remote_server >/dev/null 2>&1 || true
	$SSH "$SERVER_HOST" "nohup $RBIN --server --qps $1 --domains $2 ${DEV_ARGS[*]} > $REMOTE_DIR/srv.log 2>&1 < /dev/null & exit 0"
	for _ in $(seq 1 40); do
		$SSH "$SERVER_HOST" "grep -q listening $REMOTE_DIR/srv.log 2>/dev/null" && return 0
		sleep 0.25
	done
	return 1
}

# ---- Correctness soak (--verify): mutual-exclusion check, separate from latency. ----
if [ "$VERIFY" = 1 ]; then
	echo ">>> VERIFY: mutual-exclusion soak (software counter RMW under the CAS lock, ${ACQ_FLAG#--})"
	vfail=0
	for cell in $VERIFY_CELLS; do
		n=${cell%%:*}; d=${cell##*:}
		if ! start_cell_server "$n" "$d"; then echo "  (${n}p x ${d}dom: server never came up -- skip)"; continue; fi
		if "$BIN" --client "$SERVER_ROCE_IP" "${DEV_ARGS[@]}" --peers "$n" --domains "$d" --iters "$VERIFY_ITERS" "$ACQ_FLAG" --verify; then :; else vfail=1; fi
		kill_remote_server >/dev/null 2>&1 || true
	done
	echo
	[ "$vfail" = 0 ] && echo "=== VERIFY: ALL PASS ===" || echo "=== VERIFY: FAIL ==="
	exit "$vfail"
fi

# ---- Sweep: one CSV in sweep_plot.py's schema. ----
# Tagged by policy: a ticket run and a backoff run are different numbers and must not
# land in the same file.
TAG="rdma_$ACQUIRE"
CSV="$(csv_path "$TAG")"
echo "peers,domains,mean_us,p50_us,p90_us,p99_us,samples" > "$CSV"
echo ">>> sweep peers=[$PEERS] domains=[$DOMAINS] iters=$ITERS acquire=${ACQ_FLAG#--} -> $CSV"

for n in $PEERS; do
	for d in $DOMAINS; do
		if ! start_cell_server "$n" "$d"; then
			echo "    (${n}p x ${d}dom: server never came up -- skip)"; kill_remote_server >/dev/null 2>&1 || true; continue
		fi
		# `if !` keeps a failing cell from aborting the whole sweep (set -e).
		flog="/tmp/rdma_fail_${n}_${d}.log"
		line=$("$BIN" --client "$SERVER_ROCE_IP" "${DEV_ARGS[@]}" --peers "$n" --domains "$d" \
			--iters "$ITERS" --warmup "$WARMUP" "$ACQ_FLAG" 2>"$flog") || {
			echo "    (${n}p x ${d}dom FAILED -- see $flog)"; kill_remote_server >/dev/null 2>&1 || true; continue; }
		rm -f "$flog"
		kill_remote_server >/dev/null 2>&1 || true
		# RESULT peers=.. domains=.. mean_us=.. p50_us=.. p90_us=.. p99_us=.. tput_ops=.. samples=..
		row=$(echo "$line" | awk '{
			for (i=1;i<=NF;i++){split($i,a,"="); v[a[1]]=a[2]}
			printf "%d,%d,%.3f,%.3f,%.3f,%.3f,%d",
				v["peers"],v["domains"],v["mean_us"],v["p50_us"],v["p90_us"],v["p99_us"],v["samples"]
		}')
		echo "$row" >> "$CSV"
		printf '    %sp x %sdom -> %s\n' "$n" "$d" "$line"
	done
done

# ---- Cross-table in run_sweep's shape (drops next to CME's cross_table). ----
echo
base_note=""
[ "$BASELINE" = 1 ] && base_note=", uncontended baseline ${BASE_US}us"
echo "=== backend=rdma-$ACQUIRE ($HCA, RoCEv2 ATOMIC_HCA$base_note) -- mean_us / p99_us per (peers x domains) ==="
"$PYTHON" - "$CSV" "$TAG" <<'PYEOF'
import csv, sys
d = {}
with open(sys.argv[1]) as f:
    for r in csv.DictReader(f):
        d[(int(r['peers']), int(r['domains']))] = (float(r['mean_us']), float(r['p99_us']))
keys = sorted(d)
W = 17
def cell(t): return "%-*s" % (W, "  -" if not t else "%-7.1f %-7.1f" % t)
hdr = "%-5s %-4s | %-*s" % ("peers", "dom", W, sys.argv[2])
print(hdr); print("-" * len(hdr))
last_n = None
for (n, dom) in keys:
    if last_n is not None and n != last_n: print()
    last_n = n
    print("%-5d %-4d | %s" % (n, dom, cell(d.get((n, dom)))))
PYEOF

# ---- Figures: reuse CME's sweep_plot.py (schema matches). x=peers + x=domains. ----
FIGS=()
plot_figure() {
	local out="$1" title="$2" xmode="$3"; shift 3
	if "$PYTHON" "$HERE/../sweep/sweep_plot.py" "$@" --x "$xmode" -o "$out" --title "$title" >/dev/null 2>&1; then
		FIGS+=("$out")
	fi
}
if [ "$(wc -l < "$CSV")" -gt 1 ]; then
	for X in peers domains; do
		plot_figure "/tmp/rdma_sweep_${ACQUIRE}_${X}_$TS.png" \
			"RDMA remote lock, $ACQUIRE acquire -- RoCEv2 (x=$X)" "$X" "$TAG=$CSV"
	done
fi

echo
echo "=== CSVs ==="
if [ -e "$CSV" ]; then printf '  %s  (%s)\n' "$CSV" "$(du -h "$CSV" | cut -f1)"; else echo "  (none)"; fi

echo
echo "=== FIGURES ==="
if [ "${#FIGS[@]}" -gt 0 ]; then
	for f in "${FIGS[@]}"; do [ -e "$f" ] && printf '  %s  (%s)\n' "$f" "$(du -h "$f" | cut -f1)"; done
else
	echo "  (none)"
fi
