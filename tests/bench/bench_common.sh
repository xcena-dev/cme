# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# bench_common.sh -- what every script in this directory needs before it can run anything.
#
# Sourced, not run. It resolves the tree root from its own location, so a script here does
# not carry the relative path to it; that path moved twice while these were written, and
# each move touched all four scripts.
#
# Option parsing itself lives in ../harness/script_args.sh, because tests/baseline and
# tests/sweep take their options the same way.
#
# usage:
#   . "$(dirname "${BASH_SOURCE[0]}")/bench_common.sh"

CME="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# Overridden by --build and --slot, which every script here accepts.
BUILD=build

# Which 2 MiB window of a devdax node to use. Two runs on one device need two slots, and
# every one of them lands in the tail dax_slot_reserve holds back.
SLOT=0

. "$CME/tests/harness/script_args.sh"

# site_get / site_require / site_is_mounted. Sourced here even for a script that resolves
# no site facts of its own, so that sourcing this file is the whole preamble.
. "$CME/tests/harness/site_config.sh"

# Path to a benchmark binary by its CMake target name. Reads BUILD when called, so it works
# after the argument parse rather than only before it.
bench_bin()
{
    printf '%s' "$CME/$BUILD/tests/bench/$1"
}

# Devdax faults at PMD granularity, and a shell carrying PR_SET_THP_DISABLE cannot install
# the page. Running every backend through the wrapper costs one exec and removes the
# question. See the troubleshooting section in the top-level README.md.
bench_thp()
{
    printf '%s' "$CME/$BUILD/tests/thp_exec"
}

# Exit naming what is missing, and how to get it. A benchmark that starts and then dies on
# a missing binary has already printed a header that looks like a run.
bench_require()
{
    local path
    for path in "$@"; do
        if [ ! -x "$path" ]; then
            echo "missing $path -- build first: cmake -S $CME -B $CME/$BUILD && cmake --build $CME/$BUILD" >&2
            exit 2
        fi
    done
}
