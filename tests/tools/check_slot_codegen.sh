#!/usr/bin/env bash
# Disassemble the two slot-transfer probes and say whether the compiler kept the transfer wide.
#
# Two ways a 64 B transfer stops being one transaction: a memcpy call, whose width this build cannot
# see, or 8 B general-purpose moves, which let a reader on another host catch the line half written.
# Register width is reported rather than asserted, since a host without AVX-512 emits 4 x 16 B and
# that is the best it can do.
#
# Usage: check_slot_codegen.sh <build-dir>

set -u

buildDir=${1:-}
if [ -z "$buildDir" ]; then
    echo "usage: $0 <build-dir>" >&2
    exit 2
fi

probe="$buildDir/tests/cme-slot-codegen-probe"
if [ ! -f "$probe" ]; then
    echo "SKIP: no probe at $probe; build the cme-slot-codegen-probe target first"
    exit 77  # ctest reads 77 as skipped, the way every media-gated case here does
fi

if ! command -v objdump >/dev/null 2>&1; then
    echo "SKIP: objdump not on PATH"
    exit 77  # ctest reads 77 as skipped, the way every media-gated case here does
fi

# The question is what an optimizing compiler emits. At -O0 it spills every value to the stack and
# reloads it 8 B at a time, and --coverage adds a counter between the halves, so a wide transfer is
# not available to find and a FAIL here would name the build rather than the code.
cache="$buildDir/CMakeCache.txt"
if [ -f "$cache" ]; then
    buildType=$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$cache")
    coverage=$(sed -n 's/^CME_COVERAGE:BOOL=//p' "$cache")
    if [ "$buildType" = Debug ] || [ -z "$buildType" ] || [ "$coverage" = ON ]; then
        echo "SKIP: unoptimized build (CMAKE_BUILD_TYPE=${buildType:-<none>}, CME_COVERAGE=${coverage:-OFF})"
        exit 77  # ctest reads 77 as skipped, the way every media-gated case here does
    fi
fi

failures=0

for symbol in cmeProbeSlotSet cmeProbeSlotGet; do
    body=$(objdump --disassemble="$symbol" --no-show-raw-insn "$probe" 2>/dev/null |
        sed -n '/>:$/,/^$/p')
    if [ -z "$body" ]; then
        echo "FAIL: $symbol not found in $probe"
        failures=$((failures + 1))
        continue
    fi

    if printf '%s' "$body" | grep -qE 'call.*mem(cpy|move)'; then
        echo "FAIL: $symbol calls memcpy; the transfer width is decided at run time"
        failures=$((failures + 1))
        continue
    fi

    width=none
    if printf '%s' "$body" | grep -q 'zmm'; then
        width=64B
    elif printf '%s' "$body" | grep -q 'ymm'; then
        width=32B
    elif printf '%s' "$body" | grep -q 'xmm'; then
        width=16B
    fi

    if [ "$width" = none ]; then
        echo "FAIL: $symbol moves the line in general-purpose registers, so a reader can catch it half written"
        printf '%s\n' "$body" | sed -n '1,20p'
        failures=$((failures + 1))
        continue
    fi

    moves=$(printf '%s' "$body" | grep -cE '(v)?mov(dq|up|ap)')
    echo "OK: $symbol moves the line ${width} at a time (${moves} vector moves)"
done

if [ "$failures" -ne 0 ]; then
    echo "RESULT: FAIL ($failures)"
    exit 1
fi
echo "RESULT: PASS"
