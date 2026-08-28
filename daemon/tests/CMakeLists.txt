# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# The cmed probes. Each is its own binary, so a case that wedges or dies takes only its own probe
# down and ctest reports which one.

# ── What a probe compiles against ───────────────────────────────────────
# Directory-scoped, because the include root is a property of every probe here and not of the
# harness: a probe in a subdirectory reaches harness/ and a sibling group by the same path.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# INTERFACE, not STATIC: each probe is its own binary. The reporter arrives through cme_common.
# cme_harness is what puts tests/harness on the include path: the medium a probe runs on is named by
# a flag and resolved against the site config there, so no device path reaches this file.
add_library(cmed_test_harness INTERFACE)
target_link_libraries(cmed_test_harness INTERFACE cmed_daemon cme_harness)

# ── The layout, on its own ──────────────────────────────────────────────
# A requester and the daemon reach the layout as separate programs, so a mismatched size or
# offset is a silently misread area. This probe asserts the numbers, so it links only cmed_shared.
add_executable(cmed-abi-probe layout/abi_probe.cpp)
target_link_libraries(cmed-abi-probe PRIVATE cmed_shared cme_app_warnings)

# Both readers, so both halves of the one deployment file are held to their own keys. That is why
# this links cmed_daemon rather than cmed_shared: the daemon's reader lives there.
add_executable(cmed-config-probe runtime/config_probe.cpp)
target_link_libraries(cmed-config-probe PRIVATE cmed_client cmed_daemon cme_app_warnings)
# ctest runs from the build tree, so a relative name would resolve to nothing there.
target_compile_definitions(cmed-config-probe PRIVATE
    CMED_DAEMON_EXAMPLE_PATH="${CMED_DAEMON_EXAMPLE}"
    CMED_CLIENT_EXAMPLE_PATH="${CMED_CLIENT_EXAMPLE}"
    CMED_DEV_EXAMPLE_PATH="${CMED_DEV_EXAMPLE}")

# The accounts a config's admit keys name, resolved against this host's own databases. cmed_shared
# rather than the daemon's library: the resolution is header-only, and the reader that calls it is
# the probe above.
add_executable(cmed-account-probe runtime/account_probe.cpp)
target_link_libraries(cmed-account-probe PRIVATE cmed_shared cme_app_warnings)

# ── The area and the primitives ─────────────────────────────────────────
# No requester, so the client library is not linked. Each formats an area, which is the daemon's.
add_executable(cmed-area-probe layout/area_probe.cpp)
target_link_libraries(cmed-area-probe PRIVATE cmed_daemon cme_app_warnings)

# The fixed-width name field a record stores, on its own width rather than the area's. Header-only,
# so cmed_shared is the include path and nothing else is linked.
add_executable(cmed-field-probe layout/field_probe.cpp)
target_link_libraries(cmed-field-probe PRIVATE cmed_shared cme_app_warnings)

# The header in front of every message, on its own. handshake_probe reaches this through a real
# control loop and so only through the frames a daemon answers; the refusals are here. cmed_shared
# alone, since framing needs no area and no socket.
add_executable(cmed-message-probe layout/message_probe.cpp)
target_link_libraries(cmed-message-probe PRIVATE cmed_shared cme_app_warnings)

add_executable(cmed-sync-probe lock/sync_probe.cpp)
target_link_libraries(cmed-sync-probe PRIVATE cmed_daemon cme_app_warnings)

# The name-to-id map a session and the domain manager each keep beside the area. cmed_shared alone:
# what the map holds is a number, and nothing about a domain reaches into it.
add_executable(cmed-lookup-probe lock/lookup_probe.cpp)
target_link_libraries(cmed-lookup-probe PRIVATE cmed_shared cme_app_warnings)

add_executable(cmed-publish-probe layout/publish_probe.cpp)
target_link_libraries(cmed-publish-probe PRIVATE cmed_test_harness cme_app_warnings)

# ── The requester, against a stub daemon ────────────────────────────────
# Both halves, because a requester's exchange has no other side without the daemon's.
add_executable(cmed-lock-probe lock/lock_probe.cpp)
target_link_libraries(cmed-lock-probe PRIVATE cmed_client cmed_daemon cme_app_warnings)

# The exclusion the thread cases in cmed-lock-probe cannot reach: threads of one process would
# exclude each other through a plain mutex, and a thread that dies takes the process with it.
add_executable(cmed-crossproc-probe lock/crossproc_probe.cpp)
target_link_libraries(cmed-crossproc-probe PRIVATE cmed_client cmed_test_harness cme_app_warnings)

# The answers that are not a grant. Each runs the requester against a stub daemon that withholds
# one, so each links the client library on top of the harness.
add_executable(cmed-deadline-probe lock/deadline_probe.cpp)
target_link_libraries(cmed-deadline-probe PRIVATE cmed_client cmed_test_harness cme_app_warnings)

add_executable(cmed-refusal-probe lock/refusal_probe.cpp)
target_link_libraries(cmed-refusal-probe PRIVATE cmed_client cmed_test_harness cme_app_warnings)

add_executable(cmed-guard-probe lock/guard_probe.cpp)
target_link_libraries(cmed-guard-probe PRIVATE cmed_client cmed_test_harness cme_app_warnings)

add_executable(cmed-resolve-probe lock/resolve_probe.cpp)
target_link_libraries(cmed-resolve-probe PRIVATE cmed_client cmed_test_harness cme_app_warnings)

# The settings a caller passes, read back off the clock and off which area answered. config_probe
# stops at the struct; this one asks whether anything reads it.
add_executable(cmed-session-config-probe runtime/session_config_probe.cpp)
target_link_libraries(cmed-session-config-probe PRIVATE cmed_client cmed_test_harness cme_app_warnings)

# ── What owns a mapping, and what measures a span ───────────────────────
# Both are about a type rather than an exchange, so neither links the client library. The time
# types pull in no cmed header at all, so that probe takes the reporter and cmed_shared alone.
add_executable(cmed-mapping-probe layout/mapping_probe.cpp)
target_link_libraries(cmed-mapping-probe PRIVATE cmed_test_harness cme_app_warnings)

add_executable(cmed-time-probe runtime/time_probe.cpp)
target_link_libraries(cmed-time-probe PRIVATE cmed_shared cme_app_warnings)

# The counter a stop arrives on, the one wait over it, and the file that says one daemon runs under a
# name. cmed_shared, because all three are header-only wrappers and no case here starts a daemon.
add_executable(cmed-descriptor-probe runtime/descriptor_probe.cpp)
target_link_libraries(cmed-descriptor-probe PRIVATE cmed_shared cme_app_warnings)

# The operations shared_area.hpp defines, with nothing on top of them. cmed_shared alone, like cmed-abi-probe:
# a case that needed a session or a stub would be testing what those do with them instead.
add_executable(cmed-domain-probe lock/domain_probe.cpp)
target_link_libraries(cmed-domain-probe PRIVATE cmed_shared cme_app_warnings)

# The third layer of one domain: what this node alone knows, behind the lock that is the only way in.
# cmed_daemon, and through it libcme, because the turn an entry holds is a cme::Guard.
add_executable(cmed-local-domain-probe lock/local_domain_probe.cpp)
target_link_libraries(cmed-local-domain-probe PRIVATE cmed_daemon cme_app_warnings)

# ── The drain, raced ────────────────────────────────────────────────────
# No requester: the handler is the probe's own, because what is under test is which domains reach a
# handler and when, not what a daemon would do with them.
add_executable(cmed-drain-probe lock/drain_probe.cpp)
target_link_libraries(cmed-drain-probe PRIVATE cmed_test_harness cme_app_warnings)

# ── The threads a domain's work runs on ─────────────────────────────────
# Two probes rather than one: the pool answers whether a domain may be handed out at all, while the
# turn and the threads around it answer when a handler is reached. Neither fixture is the other's.
add_executable(cmed-workers-probe lock/workers_probe.cpp)
target_link_libraries(cmed-workers-probe PRIVATE cmed_test_harness cme_app_warnings)

add_executable(cmed-serve-loop-probe lock/serve_loop_probe.cpp)
target_link_libraries(cmed-serve-loop-probe PRIVATE cmed_test_harness cme_app_warnings)

# ── The daemon's own half ───────────────────────────────────────────────
# The only probe that opens a cme region, because it is the only one whose subject is what the
# daemon does with a CXL turn. Everything else uses StubDaemon, which takes no turn at all.
add_executable(cmed-held-domains-probe lock/held_domains_probe.cpp)
target_link_libraries(cmed-held-domains-probe PRIVATE cmed_test_harness cme_app_warnings)

# ── Handing a mapping over instead of naming it ─────────────────────────
# The transport on its own, both ends in one process. What the two processes add is nothing this can
# get wrong: the descriptor crosses the same way.
add_executable(cmed-fdpass-probe socket/fdpass_probe.cpp)
target_link_libraries(cmed-fdpass-probe PRIVATE cmed_shared cme_app_warnings)

# ── The exchange that hands the area over ───────────────────────────────
# Both halves, each running its own code. The client library is linked here and nowhere else in the
# daemon's own probes, because the last case is a real requester against a real control loop.
add_executable(cmed-handshake-probe socket/handshake_probe.cpp)
target_link_libraries(cmed-handshake-probe PRIVATE cmed_client cmed_test_harness cme_app_warnings)

# ── The receiving side, when the message does not fit ───────────────────
# fdpass_probe covers a descriptor crossing; this covers the refusals. cmed_shared alone, since a
# message that is refused never reaches a reader that would need the area or the protocol.
add_executable(cmed-seqpacket-probe socket/seqpacket_probe.cpp)
target_link_libraries(cmed-seqpacket-probe PRIVATE cmed_shared cme_app_warnings)

# ── What one message is answered with ───────────────────────────────────
# The two exchanges with no loop and no socket around them, because what handshake_probe cannot see
# through a connection is the ledger the answer left behind. A domain request is answered out of the
# manager, so this takes the harness and through it libcme.
add_executable(cmed-exchange-probe socket/exchange_probe.cpp)
target_link_libraries(cmed-exchange-probe PRIVATE cmed_test_harness cme_app_warnings)

# ── Who reaches the socket ──────────────────────────────────────────────
# No client library and no area: what is under test is the admission judgement, and a requester that
# had already been let in would be past the only thing this asks about.
add_executable(cmed-listener-probe socket/listener_probe.cpp)
target_link_libraries(cmed-listener-probe PRIVATE cmed_daemon cme_app_warnings)

# ── Two nodes on one region ─────────────────────────────────────────────
# Execs two daemons and puts a requester on each, so it needs the binary the way startup_probe does.
# The only probe in which nothing writes the registry by hand.
add_executable(cmed-multinode-probe runtime/multinode_probe.cpp)
target_link_libraries(cmed-multinode-probe PRIVATE cmed_client cmed_test_harness cme_app_warnings)
add_dependencies(cmed-multinode-probe cmed)
target_compile_definitions(cmed-multinode-probe PRIVATE CMED_BINARY_PATH="$<TARGET_FILE:cmed>")

# ── What a requester pays for a critical section ─────────────────────────
# Not registered with ctest: it is a measurement, and its grid is chosen per run rather than by a suite.
add_executable(cmed-tiered-lock-bench bench/tiered_lock_bench.cpp)
target_link_libraries(cmed-tiered-lock-bench PRIVATE cmed_client cmed_test_harness cme_app_warnings)
add_dependencies(cmed-tiered-lock-bench cmed)
target_compile_definitions(cmed-tiered-lock-bench PRIVATE CMED_BINARY_PATH="$<TARGET_FILE:cmed>")

# ── A daemon that stops without giving anything back ────────────────────
# Execs two daemons like multinode does, and kills one of them outright. Links cme as well, because the
# fact under test is participation in the region and only the inspector reads that from outside.
add_executable(cmed-crash-probe runtime/crash_probe.cpp)
target_link_libraries(cmed-crash-probe PRIVATE cmed_client cmed_test_harness cme cme_app_warnings)
add_dependencies(cmed-crash-probe cmed)
target_compile_definitions(cmed-crash-probe PRIVATE CMED_BINARY_PATH="$<TARGET_FILE:cmed>")

# ── A daemon that dies between two writes ───────────────────────────────
# Arms a named boundary instead of killing from outside, so the death lands in the gap a case names.
# Skips itself whole without CMED_FAILPOINT, where no boundary is reachable.
add_executable(cmed-failpoint-probe runtime/failpoint_probe.cpp)
target_link_libraries(cmed-failpoint-probe PRIVATE cmed_client cmed_test_harness cme_app_warnings)
add_dependencies(cmed-failpoint-probe cmed)
target_compile_definitions(cmed-failpoint-probe PRIVATE CMED_BINARY_PATH="$<TARGET_FILE:cmed>")

# ── The binary itself ───────────────────────────────────────────────────
# The one probe that execs cmed, so it is also the only one that needs the binary built and its path
# known. Everything else links the daemon's libraries and never reaches main().
add_executable(cmed-startup-probe runtime/startup_probe.cpp)
target_link_libraries(cmed-startup-probe PRIVATE cmed_test_harness cme_app_warnings)
add_dependencies(cmed-startup-probe cmed)
target_compile_definitions(cmed-startup-probe PRIVATE CMED_BINARY_PATH="$<TARGET_FILE:cmed>")

# ── Registration ────────────────────────────────────────────────────────
add_test(NAME cmed_abi_probe COMMAND cmed-abi-probe)
add_test(NAME cmed_failpoint_probe COMMAND cmed-failpoint-probe)
add_test(NAME cmed_config_probe COMMAND cmed-config-probe)
add_test(NAME cmed_account_probe COMMAND cmed-account-probe)
add_test(NAME cmed_area_probe COMMAND cmed-area-probe)
add_test(NAME cmed_field_probe COMMAND cmed-field-probe)
add_test(NAME cmed_message_probe COMMAND cmed-message-probe)
add_test(NAME cmed_sync_probe COMMAND cmed-sync-probe)
add_test(NAME cmed_lookup_probe COMMAND cmed-lookup-probe)
add_test(NAME cmed_publish_probe COMMAND cmed-publish-probe)
add_test(NAME cmed_lock_probe COMMAND cmed-lock-probe)
add_test(NAME cmed_crossproc_probe COMMAND cmed-crossproc-probe)
add_test(NAME cmed_deadline_probe COMMAND cmed-deadline-probe)
add_test(NAME cmed_refusal_probe COMMAND cmed-refusal-probe)
add_test(NAME cmed_guard_probe COMMAND cmed-guard-probe)
add_test(NAME cmed_resolve_probe COMMAND cmed-resolve-probe)
add_test(NAME cmed_session_config_probe COMMAND cmed-session-config-probe)
add_test(NAME cmed_drain_probe COMMAND cmed-drain-probe)
add_test(NAME cmed_workers_probe COMMAND cmed-workers-probe)
add_test(NAME cmed_serve_loop_probe COMMAND cmed-serve-loop-probe)
add_test(NAME cmed_mapping_probe COMMAND cmed-mapping-probe)
add_test(NAME cmed_time_probe COMMAND cmed-time-probe)
add_test(NAME cmed_descriptor_probe COMMAND cmed-descriptor-probe)
add_test(NAME cmed_domain_probe COMMAND cmed-domain-probe)
add_test(NAME cmed_local_domain_probe COMMAND cmed-local-domain-probe)
add_test(NAME cmed_held_domains_probe COMMAND cmed-held-domains-probe)
add_test(NAME cmed_fdpass_probe COMMAND cmed-fdpass-probe)
add_test(NAME cmed_seqpacket_probe COMMAND cmed-seqpacket-probe)
add_test(NAME cmed_exchange_probe COMMAND cmed-exchange-probe)
add_test(NAME cmed_listener_probe COMMAND cmed-listener-probe)
add_test(NAME cmed_handshake_probe COMMAND cmed-handshake-probe)

set_tests_properties(cmed_abi_probe cmed_config_probe cmed_account_probe cmed_area_probe
                     cmed_field_probe cmed_message_probe cmed_sync_probe cmed_lookup_probe
                     cmed_publish_probe
                     cmed_lock_probe cmed_crossproc_probe cmed_deadline_probe cmed_refusal_probe
                     cmed_guard_probe cmed_resolve_probe cmed_session_config_probe cmed_drain_probe
                     cmed_workers_probe cmed_serve_loop_probe
                     cmed_mapping_probe cmed_time_probe cmed_descriptor_probe cmed_domain_probe
                     cmed_local_domain_probe cmed_held_domains_probe
                     cmed_fdpass_probe cmed_seqpacket_probe cmed_exchange_probe cmed_listener_probe
                     cmed_handshake_probe
                     PROPERTIES TIMEOUT 30 LABELS cmed)

# Its own, because it starts a daemon per case and waits each one out, and because a build without
# CMED_FAILPOINT reports skipped rather than passing on cases it cannot reach.
set_tests_properties(cmed_failpoint_probe
                     PROPERTIES TIMEOUT 120 LABELS cmed SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

# ── The three that start a real daemon, once per medium ─────────────────
# Only these open a region through a daemon, so only these have a medium to vary. The rest reach the
# daemon's pieces directly and would assert the same thing three times.

# What a dax or uc variant gets, which is what the cme suite's media cases take. An uncacheable store
# costs orders more than an shm one, so the shm timeout would report a slow medium as a hang.
set(CMED_MEDIA_TIMEOUT 180)

# A dax window is handed out by index, and the base leaves the low ones to the cme suite: that
# directory is configured after this one, so its cursor does not exist here to extend.
set_property(DIRECTORY PROPERTY cmed_dax_next_slot 256)

# One registration on @backend, taking @windows device windows since one window holds one area. Under
# thp_exec because PR_SET_THP_DISABLE=1 breaks the devdax 2 MiB mmap with SIGBUS, daemon included.
function(cmed_media_probe name backend target windows seconds)
    get_property(slot DIRECTORY PROPERTY cmed_dax_next_slot)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:thp_exec> $<TARGET_FILE:${target}>
             --backend ${backend} --slot ${slot} --case ${name})
    # Serial, and not for speed: each publishes an area and holds the region's one peer slot for a
    # daemon it starts, so a probe sharing either at that moment would be racing a deployment mistake.
    set_tests_properties(${name} PROPERTIES
        TIMEOUT ${seconds} LABELS "cmed;${backend}" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    math(EXPR slot "${slot} + ${windows}")
    set_property(DIRECTORY PROPERTY cmed_dax_next_slot ${slot})
endfunction()

# The name each variant takes, which is the one the cme suite already uses: no infix for shm, _dax
# for a device window, _file for a uc mount. @seconds is the shm variant's alone.
function(cmed_media_probes base target windows seconds)
    cmed_media_probe(${base} shm ${target} ${windows} ${seconds})
    cmed_media_probe(${base}_dax dax ${target} ${windows} ${CMED_MEDIA_TIMEOUT})
    cmed_media_probe(${base}_file uc ${target} ${windows} ${CMED_MEDIA_TIMEOUT})
endfunction()

# startup takes two windows: one region it formats, and one the daemon must find nothing in.
cmed_media_probes(cmed_startup_probe cmed-startup-probe 2 30)
cmed_media_probes(cmed_multinode_probe cmed-multinode-probe 1 30)
# Its own timeout: the waits are the subject, judged against the region's dead window and a
# restart's own delay.
cmed_media_probes(cmed_crash_probe cmed-crash-probe 1 120)
