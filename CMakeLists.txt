# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# libcme -- CXL Mutual Exclusion (C++ redesign of libme).
#
# Layout: configuration first, then the library, then subdirectories, then
# install. Every user-facing knob lives in the CONFIGURATION block; nothing
# below it introduces a new one.

cmake_minimum_required(VERSION 3.16)
project(cme
    VERSION 0.1.0
    DESCRIPTION "CXL Mutual Exclusion (C++)"
    LANGUAGES CXX C)

include(GNUInstallDirs)

# ── Language and build defaults ─────────────────────────────────────────
# Release by default. An unspecified build type yields -O0, which silently
# tanks benchmark numbers. Override with -DCMAKE_BUILD_TYPE=...
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── CONFIGURATION ───────────────────────────────────────────────────────
# Every knob a caller may set, and nothing below this block introduces one.
#
# Site facts (which device, which mount, how big a reserve) live in config.yaml
# and are read at RUN time, by the harness. They are deliberately absent from
# this file: a build that knows the device is a build pinned to one machine, and
# it cannot be produced at all on a machine that lacks it.
#
# So everything is built and every test is registered unconditionally. config.yaml
# decides at run time what actually executes; a case whose backend is not
# configured here reports skipped rather than failed.
#
# CMake reads the same file only for knobs that genuinely change the artifact.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(SiteConfig)
cme_site_config_load("${CMAKE_CURRENT_SOURCE_DIR}/config.yaml")

# Observability axes. Each gates only its own subfunction bodies (real vs
# no-op stub); PeerDiag_t storage is allocated either way. All default OFF:
# a measurement build turns on what it needs and nothing else.
option(CME_STATS "Bump PeerDiag counters from emit() handlers" OFF)
option(CME_PROFILE "Accumulate worker CPU, publish to the SWPC profile slot" OFF)
option(CME_LOGGING "Emit stderr DBG trace lines from the ME core" OFF)
option(CME_LATENCY "Latency breakdown and trace instrumentation" OFF)
option(CME_FAILPOINT "Compile the test-only failpoint boundaries (see observe/failpoint.hpp)" OFF)

# Line and branch coverage, for answering which code no case reaches. -O0 because
# an optimised build attributes lines to whatever the inliner merged them into,
# and -fprofile-update=atomic because most cases here are multi-threaded and
# multi-process: the default non-atomic counter loses increments under both.
# A coverage run answers reachability. Timing shifts at -O0, so a fairness or
# latency verdict from this tree is not comparable to one from a Release build.
option(CME_COVERAGE "Instrument for gcov line/branch coverage (see note above)" OFF)
if(CME_COVERAGE)
    add_compile_options(-O0 -g --coverage -fprofile-update=atomic)
    add_link_options(--coverage)
endif()

# address, thread or none. The two are mutually exclusive in one binary, and they answer different
# questions: address finds a leaked mapping or fd across repeated lifecycles, thread finds a race on
# DRAM-side state. Neither sees FAM slot tearing -- that is a coherence question the sanitizers do
# not model, so a clean run here says nothing about it.
#
# -O1 and -fno-omit-frame-pointer are what makes a report name a line rather than an address, and
# -fno-optimize-sibling-calls keeps the frame that called it in the trace.
set(CME_SANITIZE "none" CACHE STRING "Sanitizer to build with: none, address, thread")
set_property(CACHE CME_SANITIZE PROPERTY STRINGS none address thread)
if(NOT CME_SANITIZE STREQUAL "none")
    if(CME_COVERAGE)
        message(FATAL_ERROR "CME_SANITIZE and CME_COVERAGE instrument the same code differently; build them separately")
    endif()
    add_compile_options(-fsanitize=${CME_SANITIZE} -fno-omit-frame-pointer -fno-optimize-sibling-calls -O1 -g)
    add_link_options(-fsanitize=${CME_SANITIZE})
endif()

# -march=native widens a 64B slot get/set to one AVX-512 transaction. With
# SSE2 the same access becomes 16B x4, and a torn 64B slot is a correctness
# problem, not a slower one. So this is semantics, not tuning -- and it ties
# the binary to the build host. Turn it off only for a portable release
# build, and re-check slot atomicity on the target ISA when you do.
if(NOT DEFINED CME_SITE_native_arch)
    set(CME_SITE_native_arch ON)
endif()
option(CME_NATIVE_ARCH "Build with -march=native (see note above)" ${CME_SITE_native_arch})

# ── Library ─────────────────────────────────────────────────────────────
set(CME_SOURCES
    src/shared.cpp

    src/admission/claim.cpp

    src/memory/memory.cpp
    src/memory/dax.cpp
    src/memory/file.cpp
    src/memory/shm.cpp

    src/core/layout/geometry.cpp
    src/core/layout/geometry_profile.cpp

    src/core/algo/ownership_transfer.cpp
    src/core/algo/lifecycle.cpp
    src/core/algo/peer.cpp
    src/core/algo/recovery.cpp

    src/core/policy/successor.cpp
    src/core/policy/successor_order.cpp
    src/core/policy/successor_peterson.cpp
    src/core/policy/successor_request.cpp
    src/core/policy/successor_request_agg.cpp
    src/core/policy/liveness_heartbeat.cpp
    src/core/policy/recovery_authority.cpp

    src/core/runtime/local_domain_view.cpp
    src/core/runtime/local_peer_state.cpp

    src/serializers/shared_session.cpp

    src/observe/inspector.cpp
    src/observe/stats.cpp
    src/observe/logging.cpp
    src/observe/latency.cpp
    src/observe/failpoint.cpp
)

add_library(cme STATIC ${CME_SOURCES})

target_include_directories(cme
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

# PRIVATE where the flag only changes library internals. PUBLIC where a
# consumer's own TU must agree: CME_PROFILE and CME_LATENCY change layouts and
# inline bodies that bench and test TUs also compile.
target_compile_definitions(cme
    PRIVATE
        $<$<BOOL:${CME_STATS}>:CME_STATS>
        $<$<BOOL:${CME_LOGGING}>:CME_LOGGING>
    PUBLIC
        $<$<BOOL:${CME_PROFILE}>:CME_PROFILE>
        $<$<BOOL:${CME_LATENCY}>:CME_LATENCY>
        $<$<BOOL:${CME_FAILPOINT}>:CME_FAILPOINT>
)

target_compile_options(cme PRIVATE
    -Wall -Wextra -Wshadow -Wpointer-arith
    -Wnon-virtual-dtor -Wold-style-cast
    -Wno-unused-parameter
)

# The ISA the 64B slot access needs, carried as its own target so that a bench which
# measures the medium directly, without linking libcme, still gets it. Every one of
# them uses AVX-512, streaming loads or clflushopt, and gcc refuses to inline those
# intrinsics without the matching -m flag.
add_library(cme_isa INTERFACE)
target_compile_options(cme_isa INTERFACE
    -mclflushopt
    $<$<BOOL:${CME_NATIVE_ARCH}>:-march=native>
)

# Which of those flags actually landed, answered per feature and carried as its own target.
include(DetectISA)

find_package(Threads REQUIRED)
# cme_isa is PUBLIC because coherency.hpp is a header: bench and test TUs inline the
# same get/set and need the same ISA to compile it.
target_link_libraries(cme PUBLIC
    cme_isa
    Threads::Threads
    rt              # shm_open, clock_gettime
)

# ── Subdirectories ──────────────────────────────────────────────────────
# The app warning set, carried as an INTERFACE target rather than a plain
# variable: a variable reads as empty in any subdirectory added before it is
# set, and does so silently. The library itself is stricter (see above).
add_library(cme_app_warnings INTERFACE)
target_compile_options(cme_app_warnings INTERFACE
    -Wall -Wextra -Wno-unused-parameter)

# Harness support: the headers that read config.yaml, and where that file lives.
# Not part of the shipped library -- libcme reads no configuration of its own.
# The path is a compile definition rather than a lookup, because ctest runs from
# the build tree and a relative "config.yaml" would resolve to nothing there.
# --config overrides it at run time.
add_library(cme_harness INTERFACE)
target_include_directories(cme_harness INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness)
target_compile_definitions(cme_harness INTERFACE
    CME_SITE_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/config.yaml")

enable_testing()
add_subdirectory(tools)
add_subdirectory(tests)   # cases, and the bench/baseline subtrees it owns
add_subdirectory(examples)

# ── Install ─────────────────────────────────────────────────────────────
set_target_properties(cme PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME cme)

install(TARGETS cme
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/cme DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/libcme.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/libcme.pc
    @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libcme.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ── Configuration summary ───────────────────────────────────────────────
# Printed so a build's knobs are visible in the log, not only in the cache.
message(STATUS "libcme ${PROJECT_VERSION}")
message(STATUS "  C++${CMAKE_CXX_STANDARD}, ${CMAKE_BUILD_TYPE}, coherency=runtime (cme::CoherencyMode)")
message(STATUS "  native arch : ${CME_NATIVE_ARCH}")
if(CME_SITE_FOUND)
    message(STATUS "  site config : ${CMAKE_CURRENT_SOURCE_DIR}/config.yaml")
else()
    message(STATUS "  site config : none (copy config.example.yaml to config.yaml); "
                   "dax and uc cases will report skipped")
endif()
message(STATUS "  observe     : stats=${CME_STATS} profile=${CME_PROFILE} logging=${CME_LOGGING} latency=${CME_LATENCY}")
message(STATUS "  sanitize    : ${CME_SANITIZE}")
message(STATUS "  failpoint   : ${CME_FAILPOINT}")
if(CME_COVERAGE)
    message(STATUS "  coverage    : ON (-O0; reachability, not timing)")
endif()
