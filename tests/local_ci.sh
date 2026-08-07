#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# local_ci.sh -- everything a hosted runner cannot prove.
#
# GitHub Actions has no devdax node and no uncacheable mount, so it can only ever run the
# shm third of the suite. This runs the whole of it on a machine that has the hardware.
#
# A missing medium is a FAILURE here, which is the opposite of the rule everywhere else in
# this tree. Elsewhere skipping is right: a developer without CXL should still get a green
# ctest. Here it would defeat the point -- a run that skips dax and uc proves exactly what
# the hosted runner already proved, and then says PASS. --allow-missing downgrades that to
# a warning for the case where you knowingly want the rest.
#
# The preflight runs first and refuses early, because finding out about a missing mount
# after a fifteen-minute suite is worse than not running it.
#
# tests/baseline is not here. It needs a second host over RoCE, which is a dependency of a
# different kind from "this machine has a devdax node", and a gate should not require one.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CME="$(cd "$HERE/.." && pwd)"
. "$HERE/harness/script_args.sh"
. "$HERE/harness/site_config.sh"

USAGE="usage: local_ci.sh [--build DIR] [--jobs N] [--full] [--allow-missing] [--python BIN]
  --full           run the sweeps at their real counts instead of cut down
  --allow-missing  warn instead of failing when a medium or tool is absent
  --jobs           ctest parallelism; 1 by default, because a gate that flakes is not a gate"

BUILD=build-ci
JOBS=1
FULL=0
ALLOW_MISSING=0
PYTHON=python3
script_parse_args "$USAGE" "build jobs python" "full allow-missing" "$@"

STARTED_AT=$(date +%s)
FAILED_STAGES=""
MISSING=""

# ── reporting ───────────────────────────────────────────────────────────

banner()
{
	echo
	echo "==================== $* ===================="
}

# Record a missing prerequisite. Whether that ends the run is decided once, after the whole
# preflight, so one run reports every hole rather than one per invocation.
want()
{
	MISSING="$MISSING
  $*"
}

# Run a stage, remember whether it failed, and keep going. A gate that stops at the first
# failure hides how much else is broken, and the summary is what gets read.
stage()
{
	local name="$1"
	shift
	banner "$name"
	local began
	began=$(date +%s)
	if "$@"; then
		echo "--- $name: ok ($(($(date +%s) - began))s)"
	else
		echo "--- $name: FAILED ($(($(date +%s) - began))s)" >&2
		FAILED_STAGES="$FAILED_STAGES $name"
	fi
}

# ── preflight ───────────────────────────────────────────────────────────

preflight()
{
	local tool
	for tool in cmake gawk "$PYTHON"; do
		command -v "$tool" >/dev/null || want "$tool is not on PATH"
	done
	"$PYTHON" -c 'import matplotlib' 2>/dev/null || want "$PYTHON has no matplotlib (the plotters need it)"

	if [ ! -f "$CME/config.yaml" ]; then
		want "config.yaml is absent; copy config.example.yaml and fill it in"
		return
	fi

	local device mount
	device="$(site_get dax_device)"
	if [ -z "$device" ]; then
		want "dax_device is empty in config.yaml"
	elif [ ! -c "$device" ]; then
		want "dax_device '$device' is not a character device"
	fi

	mount="$(site_get file_backend_dir)"
	if [ -z "$mount" ]; then
		want "file_backend_dir is empty in config.yaml"
	elif ! site_is_mounted "$mount"; then
		want "file_backend_dir '$mount' is not a mount point"
	fi
}

report_preflight()
{
	echo "tree      : $CME"
	echo "build     : $CME/$BUILD"
	echo "dax_device: $(site_get dax_device)"
	echo "file dir  : $(site_get file_backend_dir)"
	echo "ctest jobs: $JOBS"
	echo "depth     : $([ "$FULL" = 1 ] && echo "full counts" || echo "cut down (--full for the real thing)")"

	if [ -z "$MISSING" ]; then
		echo
		echo "preflight : every medium and tool is present"
		return 0
	fi

	echo
	echo "preflight found:$MISSING" >&2
	if [ "$ALLOW_MISSING" = 1 ]; then
		echo
		echo "carrying on because --allow-missing: this run proves less than a full one" >&2
		return 0
	fi
	echo
	echo "refusing to run: this script exists to cover what a hosted runner cannot," >&2
	echo "and a run without those media would only repeat what it already covers." >&2
	echo "Pass --allow-missing to run the rest anyway." >&2
	return 1
}

# ── stages ──────────────────────────────────────────────────────────────

do_build()
{
	# Instrumentation on, so the axes compile and cme-top's CPU columns have data. They
	# change inline bodies and one slot's layout, which is exactly why they need building.
	#
	# CME_FAILPOINT too, because the failpoint cases skip themselves without it and a hosted
	# runner has no reason to carry an axis that kills its own processes.
	cmake -S "$CME" -B "$CME/$BUILD" -DCME_STATS=ON -DCME_PROFILE=ON -DCME_FAILPOINT=ON >/dev/null &&
		cmake --build "$CME/$BUILD" -j
}

do_suite()
{
	ctest --test-dir "$CME/$BUILD" -j "$JOBS" --output-on-failure
}

# The scripts, not the numbers. Three of the four benchmarks link no cme target at all --
# they measure the medium with mmap and intrinsics -- so no change to this library can move
# what they report. What can break is the plumbing: which target they resolve, which slot
# they land on, whether a phase skips or errors. That is what running them at their
# smallest useful counts checks, and nothing else in the tree checks it.
#
# recovery_latency_bench is the exception: it drives libcme, and it exits non-zero when a
# recovery fails to finish inside its window.
do_scripts()
{
	local failed=0
	if [ "$FULL" = 1 ]; then
		"$HERE/bench/cacheline_bench.sh" --build "$BUILD" || failed=1
		"$HERE/bench/read_tail_bench.sh" --build "$BUILD" || failed=1
		"$HERE/bench/lww_settle_bench.sh" --build "$BUILD" || failed=1
		"$HERE/bench/recovery_latency_bench.sh" --build "$BUILD" --backend dax || failed=1
		"$HERE/sweep/run_sweep.sh" --build "$BUILD" --python "$PYTHON" || failed=1
		"$HERE/sweep/stress_recovery.sh" --bin "$CME/$BUILD/tests/cme-recovery-test" \
			--runs 50 || failed=1
	else
		"$HERE/bench/cacheline_bench.sh" --build "$BUILD" --iters 50000 --slot 3 || failed=1
		"$HERE/bench/read_tail_bench.sh" --build "$BUILD" --iters 100000 --slot 3 || failed=1
		"$HERE/bench/lww_settle_bench.sh" --build "$BUILD" --contenders "2 4" \
			--repeats 50 --slot 3 || failed=1
		"$HERE/bench/recovery_latency_bench.sh" --build "$BUILD" --backend dax \
			--strategies request --peers 4 --domains 1 || failed=1
		"$HERE/sweep/run_sweep.sh" --build "$BUILD" --python "$PYTHON" \
			--no-seqlat --no-tierlat || failed=1
		"$HERE/sweep/stress_recovery.sh" --bin "$CME/$BUILD/tests/cme-recovery-test" \
			--runs 3 || failed=1
	fi
	return "$failed"
}

# ── run ─────────────────────────────────────────────────────────────────

banner "preflight"
preflight
report_preflight || exit 2

stage build "do_build"
# Nothing downstream means anything against a tree that did not build.
if [ -n "$FAILED_STAGES" ]; then
	echo
	echo "build failed; stopping before the suite" >&2
	exit 1
fi

stage suite "do_suite"
stage scripts "do_scripts"

banner "summary"
echo "elapsed: $(($(date +%s) - STARTED_AT))s"
if [ -n "$FAILED_STAGES" ]; then
	echo "FAILED:$FAILED_STAGES" >&2
	exit 1
fi
echo "every stage passed"
