# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# bench/ -- the microbenchmarks behind the numbers in docs/design.
#
# Each of these is a build target, so a normal build catches whether it still
# compiles. Without a target, a header change could break one silently, and
# the next person to need a measurement would find a broken tool.
#
# None is registered with ctest. A benchmark has no pass or fail, and a machine
# without the device it wants should not report a failure for saying so; each
# one resolves its target from config.yaml and exits with a message when the
# entry is empty.
#
# cme_harness carries the config.yaml path and the headers that read it and name
# the area a run owns. Every target here links libcme as well, so the tree has one
# implementation of naming, clearing, removal, and the open/ftruncate/mmap rules
# each backend imposes. The three probes still time their own loads and stores on
# a plain pointer, so what they measure is unchanged by the link.

# A timed loop here runs in a nanosecond or two, and Zen fetches from the op cache in
# 32-byte windows, so a loop that starts mid-window loses front-end bandwidth. Without
# this, an edit that shifts code by 16 bytes moves the sub-nanosecond numbers by half a
# nanosecond, which reads as a result. Measured on cacheline_bench: 0.5 to 1.0 ns/iter on
# the scalar-load case, from alignment alone.
set(CME_BENCH_ALIGN -falign-loops=32)

# One entry point for the probes, because the link line and the alignment flag repeat
# otherwise. REQUIRES names ISA targets from cmake/DetectISA.cmake, and each one both gates
# the target and lands on its link line, so a requirement is written once.
function(cme_add_bench)
    cmake_parse_arguments(ARG "" "NAME;SOURCE" "LIBS;REQUIRES" ${ARGN})
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "cme_add_bench: unrecognised argument ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    foreach(feature IN LISTS ARG_REQUIRES)
        if(NOT TARGET ${feature})
            message(STATUS "${ARG_NAME}: not built, this build's ISA lacks ${feature}")
            return()
        endif()
    endforeach()
    add_executable(${ARG_NAME} ${ARG_SOURCE})
    target_link_libraries(${ARG_NAME} PRIVATE
        cme cme_app_warnings cme_harness cme_isa ${ARG_REQUIRES} ${ARG_LIBS})
    target_compile_options(${ARG_NAME} PRIVATE ${CME_BENCH_ALIGN})
endfunction()

cme_add_bench(NAME cme-cacheline-bench SOURCE cacheline_bench.cpp)

# read_tail_bench times one 512-bit load, so that load is the measurement rather than one
# probe among several. Reading the same line as eight scalar loads would answer a different
# question, so the target is absent where the ISA cannot hold the vector. cacheline_bench
# needs no gate: its 512-bit cases sit beside scalar and 256-bit ones and drop out alone.
cme_add_bench(NAME cme-read-tail-bench SOURCE read_tail_bench.cpp
              REQUIRES cme_avx512
              LIBS Threads::Threads)

# Sets ClaimSettle in src/config.hpp. A port to other FAM hardware re-runs this.
cme_add_bench(NAME cme-lww-settle-bench SOURCE lww_settle_bench.cpp
              LIBS Threads::Threads)

# Unlike the three above, this one drives libcme itself: it times recovery, so it needs the
# peer machinery and the test harness that picks a backend. It reaches no medium directly, so
# it takes neither cme_isa nor the loop alignment the timed probes need.
add_executable(cme-recovery-latency-bench recovery_latency_bench.cpp)
target_link_libraries(cme-recovery-latency-bench PRIVATE cme cme_app_warnings cme_harness)
