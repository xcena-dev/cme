#!/usr/bin/env bash
# stress_recovery.sh — long-run loop for cme-recovery-test.
#
# Log policy:
#   - Each run's output is streamed to the out dir's current.log, overwritten every run.
#   - On PASS: nothing extra saved.
#   - On FAIL: current.log is copied to fail-RNNNNN.log and, by default, the loop HALTS so
#     the next dev can investigate. --no-halt-on-fail keeps going.

set -u

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
. "$HERE/../harness/script_args.sh"

USAGE="usage: stress_recovery.sh [--runs N|forever] [--bin PATH] [--out-dir DIR]
                         [--timeout SECONDS] [--no-halt-on-fail]"

RUNS=100
BIN="$HERE/../../build/tests/cme-recovery-test"
OUT_DIR=/tmp/cme_stress
TIMEOUT=60
HALT_ON_FAIL=1
script_parse_args "$USAGE" "runs bin out-dir timeout" "halt-on-fail" "$@"

case "$RUNS" in
    forever | inf) N=0 ;;
    '' | *[!0-9]*) echo "$0: --runs takes a count or 'forever' (got '$RUNS')" >&2; exit 2 ;;
    *) N="$RUNS" ;;
esac

if [[ ! -x "$BIN" ]]; then
    echo "missing: $BIN" >&2
    exit 2
fi

mkdir -p "$OUT_DIR"

PASS=0
FAIL=0
RUN=0
START=$(date +%s)
CUR="$OUT_DIR/current.log"

summary() {
    local now elapsed
    now=$(date +%s)
    elapsed=$((now - START))
    echo ""
    echo "================ stress summary ================"
    echo "runs   : $RUN"
    echo "pass   : $PASS"
    echo "fail   : $FAIL"
    if (( RUN > 0 )); then
        echo "fail%  : $(awk -v f="$FAIL" -v r="$RUN" 'BEGIN{printf "%.2f", (f*100.0)/r}')"
    fi
    echo "elapsed: ${elapsed}s"
    echo "logs   : $OUT_DIR"
    if (( FAIL > 0 )); then
        echo "fail logs:"
        ls "$OUT_DIR"/fail-R*.log 2>/dev/null | head -20
    fi
    echo "================================================"
}

trap 'summary; exit 0' INT TERM
trap 'summary' EXIT

while :; do
    RUN=$((RUN + 1))
    timeout "$TIMEOUT" "$BIN" > "$CUR" 2>&1
    RC=$?

    if grep -q "^RESULT: PASS" "$CUR"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAIL_LOG="$OUT_DIR/fail-R$(printf '%05d' "$RUN").log"
        cp "$CUR" "$FAIL_LOG"
        echo ""
        echo "[RUN $RUN] FAIL (rc=$RC) → $FAIL_LOG"
        # show the FAIL invariant line + per-peer DIAG
        grep -E "^  FAIL |^DIAG |^RESULT:" "$CUR"
        echo ""
        if (( HALT_ON_FAIL )); then
            echo "[HALT] stopping after the first failure; --no-halt-on-fail keeps going"
            break
        fi
    fi

    if (( RUN % 10 == 0 )); then
        elapsed=$(( $(date +%s) - START ))
        echo "[progress] run=$RUN pass=$PASS fail=$FAIL elapsed=${elapsed}s"
    fi

    if (( N > 0 && RUN >= N )); then
        break
    fi
done

# clean up current.log so the out dir holds only fail logs
rm -f "$CUR"
