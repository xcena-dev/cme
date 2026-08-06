# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# tests/ -- test executables and their ctest registrations.
# enable_testing() is called by the top-level CMakeLists.
#
# Every case is declared once, in CASES below, and cme_case() registers it on
# each backend it applies to, so a case cannot exist on one backend while
# silently missing from another.
#
# Backends:
#   shm   POSIX shared memory. No configuration, always registered.
#   dax   CXL devdax chardev, one 2 MiB window per test. Needs config.yaml's dax_device.
#   uc    one file per test on an uncacheable mount. Needs config.yaml's file_backend_dir.
#
# Both device knobs default from config.yaml; empty means the backend is
# skipped, not that the build fails.

# ── Configuration ───────────────────────────────────────────────────────

# Every dax test gets its own 2 MiB window on the device, so none of them shares a byte with
# another and no resource lock is needed -- `ctest -j` alone decides how many run at once.
#
# What CMake hands out is a slot INDEX, not a byte offset. Turning an index into an offset
# needs the device size, which is a fact about the machine a test runs on rather than about
# the tree it was built in. helper.hpp resolves it at run time (see daxUri), so configure
# never touches the device, a build without one still registers the shm suite, and one build
# tree can be pointed at different devices.
#
# Nothing else about the device appears in this file. Which device, which mount, how
# large a reserve: all of it is read from config.yaml at run time. Every case is
# registered on every medium unconditionally, and one whose backend is not configured
# on this machine reports skipped (SKIP_RETURN_CODE below) rather than failed.

# The four successor policies, and the infix each one contributes to a test
# name. request-agg shortens to agg, which is why this is a map and not a list.
set(CME_STRATEGY_MAP
    "order|order"
    "request|request"
    "request-agg|agg"
    "peterson|peterson")

# ── Registration helpers ────────────────────────────────────────────────

# THP wrapper. A harness that inherits PR_SET_THP_DISABLE=1 breaks the devdax 2 MiB-aligned
# mmap with SIGBUS, so every FAM case below runs the target through this. Links nothing:
# it clears the flag and exec()s.
add_executable(thp_exec harness/thp_exec.cpp)

# Build a test executable linked against cme. @src is resolved against cases/, so the
# table below reads as case names rather than paths. Extra sources after @src are compiled
# in as given, for harness code that is not part of the shipped library.
function(cme_test_exe name src)
    add_executable(${name} cases/${src} ${ARGN})
    target_link_libraries(${name} PRIVATE cme cme_app_warnings cme_harness)
endfunction()

# Slot cursor. A directory property rather than a variable, because a variable
# would have to be hand-propagated back through every PARENT_SCOPE it crosses,
# and would silently hand out slot 0 forever the first time someone called
# these helpers from inside another function.
set_property(DIRECTORY PROPERTY cme_dax_next_slot 0)

# Register a case on a FAM medium: @target (+ args) under thp_exec, with the harness
# flags that select the backend. The uc variant is handed a slot too; it goes unused,
# since a uc case is isolated by its own file rather than by an offset.
function(cme_media_test name backend target)
    get_property(slot DIRECTORY PROPERTY cme_dax_next_slot)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:thp_exec> ${target}
             --backend ${backend} --slot ${slot} --case ${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 180 LABELS ${backend} SKIP_RETURN_CODE 77)
    math(EXPR slot "${slot} + 1")
    set_property(DIRECTORY PROPERTY cme_dax_next_slot ${slot})
endfunction()

# A uc case leaves its file behind: none of these binaries unlink their own, and a
# stale pre-sized file is WORM-rejected by the next run's ftruncate (see file.cpp).
# The cleanup runs the same binary with --cleanup, because only the harness knows
# where the mount is -- CMake deliberately does not.
function(cme_uc_test name target)
    cme_media_test(${name} uc ${target} ${ARGN})
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED ${name}_fixture)
    add_test(NAME ${name}_cleanup COMMAND ${target} --backend uc --case ${name} --cleanup)
    set_tests_properties(${name}_cleanup PROPERTIES FIXTURES_CLEANUP ${name}_fixture LABELS uc)
endfunction()

# cme_case(<base> TARGET <exe> [options]) -- register one case on every backend
# it applies to.
#
#   STRATEGIES        expand over all four policies, passed as --strategy.
#                     The name gains the policy infix: <base>_order, _request,
#                     _agg, _peterson.
#   STRATEGY <one>    fix one policy; the name gains no infix.
#   (neither)         the binary picks its own policy.
#   ARGS <args...>    command-line arguments, identical on every backend.
#   BACKENDS <list>   default "shm dax uc".
#   SHM_TIMEOUT <n>   ctest timeout for the shm variant, default 120. The dax
#                     and uc variants use 180, set by cme_media_test.
#   LABELS <label>    label for the shm variant. Unlabelled shm tests are
#                     swept into the shm label at the end of this file.
#   UC_VIA_DAX        register the uc variant without the cleanup fixture, for
#                     a binary that already unlinks its own file.
#
# Names follow <base>[_policy][_dax|_file], which is what the existing suites and the
# report already assume.
function(cme_case base)
    cmake_parse_arguments(C
        "STRATEGIES;UC_VIA_DAX"
        "TARGET;STRATEGY;SHM_TIMEOUT;LABELS"
        "ARGS;BACKENDS"
        ${ARGN})
    if(NOT C_TARGET)
        message(FATAL_ERROR "cme_case(${base}): TARGET is required")
    endif()
    if(NOT C_BACKENDS)
        set(C_BACKENDS shm dax uc)
    endif()
    if(NOT C_SHM_TIMEOUT)
        set(C_SHM_TIMEOUT 120)
    endif()

    # Variant table. "@" stands for an empty field: a CMake list cannot carry
    # an empty element through foreach() reliably, so the sentinel is explicit.
    set(infixes "")
    set(policies "")
    if(C_STRATEGIES)
        foreach(entry IN LISTS CME_STRATEGY_MAP)
            string(REPLACE "|" ";" pair "${entry}")
            list(GET pair 0 policy)
            list(GET pair 1 infix)
            list(APPEND infixes "_${infix}")
            list(APPEND policies "${policy}")
        endforeach()
    elseif(C_STRATEGY)
        list(APPEND infixes "@")
        list(APPEND policies "${C_STRATEGY}")
    else()
        list(APPEND infixes "@")
        list(APPEND policies "@")
    endif()

    list(LENGTH infixes variant_count)
    math(EXPR last "${variant_count} - 1")
    foreach(i RANGE ${last})
        list(GET infixes ${i} infix)
        list(GET policies ${i} policy)
        if(infix STREQUAL "@")
            set(infix "")
        endif()
        set(policy_args "")
        if(NOT policy STREQUAL "@")
            set(policy_args --strategy ${policy})
        endif()
        set(name "${base}${infix}")

        if("shm" IN_LIST C_BACKENDS)
            add_test(NAME ${name}
                COMMAND ${C_TARGET} --backend shm --case ${name} ${policy_args} ${C_ARGS})
            set_tests_properties(${name} PROPERTIES
                TIMEOUT ${C_SHM_TIMEOUT} SKIP_RETURN_CODE 77)
            if(C_LABELS)
                set_tests_properties(${name} PROPERTIES LABELS ${C_LABELS})
            endif()
        endif()

        if("dax" IN_LIST C_BACKENDS)
            cme_media_test(${name}_dax dax $<TARGET_FILE:${C_TARGET}> ${policy_args} ${C_ARGS})
        endif()

        if("uc" IN_LIST C_BACKENDS)
            if(C_UC_VIA_DAX)
                cme_media_test(${name}_file uc $<TARGET_FILE:${C_TARGET}> ${policy_args} ${C_ARGS})
            else()
                cme_uc_test(${name}_file $<TARGET_FILE:${C_TARGET}> ${policy_args} ${C_ARGS})
            endif()
        endif()
    endforeach()
endfunction()

# ── Executables ─────────────────────────────────────────────────────────
cme_test_exe(cme-region-smoke-test test_region_smoke.cpp)
cme_test_exe(cme-fairness-smoke-test test_fairness_smoke.cpp)
cme_test_exe(cme-fairness-test test_fairness.cpp)
cme_test_exe(cme-recovery-test test_recovery.cpp)
cme_test_exe(cme-shared-smoke-test test_shared_smoke.cpp)
cme_test_exe(cme-util-test test_util.cpp)
cme_test_exe(cme-admission-race-test test_admission_race.cpp)
cme_test_exe(cme-participation-test test_participation.cpp)
cme_test_exe(cme-domain-lifecycle-test test_domain_lifecycle.cpp)
cme_test_exe(cme-create-race-test test_create_race.cpp)
cme_test_exe(cme-recovery-resume-test test_recovery_resume.cpp)
cme_test_exe(cme-recovery-stake-gate-test test_recovery_stake_gate.cpp)
cme_test_exe(cme-recovery-zombie-claim-test test_recovery_zombie_claim.cpp)
cme_test_exe(cme-recovery-orphan-test test_recovery_orphan.cpp)
cme_test_exe(cme-admission-stress-test test_admission_stress.cpp)
cme_test_exe(cme-orphan-churn-test test_orphan_churn.cpp)
cme_test_exe(cme-agg-recovery-test test_agg_recovery.cpp)
cme_test_exe(cme-mutex-test test_mutual_exclusion.cpp)
cme_test_exe(cme-readmit-gate-test test_readmit_gate.cpp)
cme_test_exe(cme-departure-strand-test test_departure_strand.cpp)
cme_test_exe(cme-seq-latency-test test_seq_latency.cpp)
cme_test_exe(cme-tiered-lock-test test_tiered_lock.cpp)
cme_test_exe(cme-shared-session-test test_shared_session.cpp)
cme_test_exe(cme-shared-lifecycle-test test_shared_lifecycle.cpp)
cme_test_exe(cme-region-reject-test test_region_reject.cpp)
cme_test_exe(cme-api-contract-test test_api_contract.cpp)
cme_test_exe(cme-api-reject-test test_api_reject.cpp)
cme_test_exe(cme-region-corrupt-test test_region_corrupt.cpp)
cme_test_exe(cme-lease-steal-test test_lease_steal.cpp)
cme_test_exe(cme-memory-reject-test test_memory_reject.cpp)
cme_test_exe(cme-mutex-baseline-test test_mutex_baseline.cpp)
cme_test_exe(cme-shadow-authority-test test_shadow_authority.cpp)
cme_test_exe(cme-coherency-modes-test test_coherency_modes.cpp)
cme_test_exe(cme-slot-inherit-test test_slot_inherit.cpp)
cme_test_exe(cme-domain-incarnation-test test_domain_incarnation.cpp)
cme_test_exe(cme-file-presized-test test_file_presized.cpp)

# ── CASES ───────────────────────────────────────────────────────────────

# No backend axis at all, so these sit outside the shm/dax/uc matrix rather than
# showing up as shm with two excluded cells. mutex_baseline is a process-shared
# pthread_mutex (the coherent-DRAM floor, not a cme region); util_test drives
# util/endian.hpp + util/coherency.hpp over an anonymous mapping and never opens a
# Geometry; region_smoke asserts POSIX-shm name lifecycle (shm_unlink, then a joiner
# must fail), a property neither devdax nor a uc mount has.
cme_case(region_smoke TARGET cme-region-smoke-test BACKENDS shm SHM_TIMEOUT 60 LABELS common)
cme_case(util_test TARGET cme-util-test BACKENDS shm SHM_TIMEOUT 60 LABELS common)
# --verify is the correctness soak (lost-update check); the latency sweep itself has no
# pass/fail and is not a ctest concern. See run_mutex_sweep.sh for the comparison.
cme_case(mutex_baseline TARGET cme-mutex-baseline-test BACKENDS shm SHM_TIMEOUT 60 LABELS common
    ARGS --verify -n 4 -d 2 -i 200 --warmup 20)

# Header rejection: the one case that hands the library a region it did not format. shm because
# the mappedSize-below-area check needs a mapping sized to the region, which neither a fixed
# 2 MiB dax window nor a pre-sized uc file is. The checks themselves read header bytes, so one
# medium answers for all three.
cme_case(region_reject TARGET cme-region-reject-test BACKENDS shm SHM_TIMEOUT 60)

# Backend rejection: bad URIs, names that cannot exist, sizes that cannot be mapped, a devdax
# offset off its PMD boundary. shm because the case builds its own URIs and scratch files instead
# of using the medium it was handed, so a dax or uc variant would repeat identical assertions.
cme_case(memory_reject TARGET cme-memory-reject-test BACKENDS shm SHM_TIMEOUT 60)

# The file backend's creator-sizing rule, both answers: a fresh file is grown to the mapping and a
# pre-sized one is left alone, which is what a WORM mount requires. shm for the same reason as
# memory_reject -- the case seals its own memfd and never opens the medium it was handed.
cme_case(file_presized TARGET cme-file-presized-test BACKENDS shm SHM_TIMEOUT 60)

# Smoke and lifecycle: no policy axis, every backend.
cme_case(fairness_smoke TARGET cme-fairness-smoke-test SHM_TIMEOUT 60)
cme_case(shared_smoke TARGET cme-shared-smoke-test SHM_TIMEOUT 60)
cme_case(admission_race TARGET cme-admission-race-test SHM_TIMEOUT 60)
cme_case(participation TARGET cme-participation-test SHM_TIMEOUT 60)
cme_case(domain_lifecycle TARGET cme-domain-lifecycle-test SHM_TIMEOUT 60)
cme_case(create_race TARGET cme-create-race-test SHM_TIMEOUT 60)
cme_case(admission_stress TARGET cme-admission-stress-test)

# Recovery family. All four policies for parity: the paths under test are
# policy-independent (shared recovery FSM, RA claim path, ghost sweep), so a
# divergence between policies is itself the signal.
cme_case(recovery TARGET cme-recovery-test STRATEGIES)
cme_case(recovery_resume TARGET cme-recovery-resume-test STRATEGIES)
cme_case(recovery_stake_gate TARGET cme-recovery-stake-gate-test STRATEGIES)
cme_case(recovery_zombie_claim TARGET cme-recovery-zombie-claim-test STRATEGIES)
cme_case(recovery_orphan TARGET cme-recovery-orphan-test STRATEGIES)
cme_case(orphan_churn TARGET cme-orphan-churn-test STRATEGIES)

# RequestAgg aggregator re-election on crash; the policy is fixed inside the binary.
cme_case(agg_recovery TARGET cme-agg-recovery-test)

# C1 readmission gate: a dead peer's slot is re-claimable only after recovery marks it
# None. Policy-independent cleanup path, so one policy suffices.
cme_case(readmit_gate TARGET cme-readmit-gate-test STRATEGY request SHM_TIMEOUT 60)

# C4 departure strand: repeated real departures under contention must never leave a
# domain record naming the peer that left. ITERS raises the departure count.
cme_case(departure_strand TARGET cme-departure-strand-test STRATEGY order)

# Mutual exclusion (B2): no lost update under contention, plus bounded wait.
# The no-lost-update property holds for ALL policies. The bounded-wait assert is
# effectively a hard fairness demand (every peer finishes M acquires within the 1s
# acquire deadline at 16 peers / 1 domain). peterson (tournament = structural bounded
# wait) and request-agg (the aggregation tree absorbs contention) meet it; order
# (token-ring) and request (hand-raise) do not, because their hold-until-handed-off
# design starves the tail under heavy single-domain contention. Raising the deadline
# 10x does not let order complete, so that is genuine starvation rather than slow
# throughput. The equity property is covered with relaxed bounds by the fairness test.
# mutex_order and mutex_request are therefore KNOWN-FAIL until the handoff policies get
# a structural fairness fix (fair queue, hold cap, or unconditional yield).
cme_case(mutex TARGET cme-mutex-test STRATEGIES)

# SharedSession: the public intra-node tier. One session, N threads, exclusion must
# hold -- the case a bare Session does not cover, since the token is per peer.
cme_case(shared_session TARGET cme-shared-session-test STRATEGIES)

# SharedSession's exit side: leaveDomain, deleteDomain, and the ownership drop both of them run.
# One policy suffices -- participation and the domain registry sit above the successor policy, so
# the four would exercise the same code four times. Also the only case that reaches the OpenOpts_t
# overload of open, Guard's move constructor, and the public cme::flush.
cme_case(shared_lifecycle TARGET cme-shared-lifecycle-test STRATEGY request SHM_TIMEOUT 60)

# Declared contracts nothing else reaches: move assignment on Session and Guard, and that
# makeSuccessorPolicy hands back the kind it was asked for. One policy, because the factory check
# walks all four itself and the move-assignment half does not depend on which one is installed.
cme_case(api_contract TARGET cme-api-contract-test STRATEGY request SHM_TIMEOUT 60)

# The refusal side of the same API: dimensions format rejects, the not-joined guard on a moved-from
# handle, domain ids and names Peer rejects, and the two lifecycle answers that mean "already so".
# One policy, because argument validation sits above the successor policy entirely.
cme_case(api_reject TARGET cme-api-reject-test STRATEGY request SHM_TIMEOUT 60)

# The slot-level magic checks below the header: member slot, control record, a data record, and the
# strategy field. One policy, because each check fails before the successor policy is built or
# reaches code that does not consult it.
cme_case(region_corrupt TARGET cme-region-corrupt-test STRATEGY request SHM_TIMEOUT 60)

# The shadow is a hint and the truth line decides: a shadow forged to name the waiter must not
# hand it the domain. One policy, because waitForOwnership sits below the successor policy and
# every one of them reaches the same wait.
cme_case(shadow_authority TARGET cme-shadow-authority-test STRATEGY request SHM_TIMEOUT 60)

# All three coherency regimes over one medium, with the mode named by the case rather than taken
# from the backend. shm only: it is the medium every machine has, and carrying all three there is
# the whole point -- on a hosted runner nothing else executes the Flush barrier path.
cme_case(coherency_modes TARGET cme-coherency-modes-test STRATEGY request BACKENDS shm
    SHM_TIMEOUT 60)

# A member slot's next occupant inherits no participation from the peer that left cleanly.
# readmit_gate covers the dead-peer route to the same bits; one policy, because membership sits
# above the successor policy.
cme_case(slot_inherit TARGET cme-slot-inherit-test STRATEGY request SHM_TIMEOUT 60)

# A reused domain slot is a new incarnation: generation rises, epoch does not reset, and a peer
# holding the old view is refused until it re-joins. One policy, for the same reason as
# domain_lifecycle -- the registry sits above the successor policy.
cme_case(domain_incarnation TARGET cme-domain-incarnation-test STRATEGY request SHM_TIMEOUT 60)

# The admission lease's failure side: a stalled lease stolen after LeaseTimeout, and a lease that
# keeps moving until the joiner's deadline runs out. shm only -- both cases assert wall-clock
# windows on one 64 B line and read no other medium state.
cme_case(lease_steal TARGET cme-lease-steal-test STRATEGY request BACKENDS shm SHM_TIMEOUT 60)

# Two-tier lock: SharedSession's per-domain mutex (intra-node) over CXL ownership
# (inter-node), N sessions x T threads each. Only the outer region switches backend;
# the inner tier is a std::mutex in DRAM. UC_VIA_DAX because this binary unlinks its
# own FAM file, so the cleanup fixture would be redundant.
cme_case(tiered TARGET cme-tiered-lock-test STRATEGIES UC_VIA_DAX)

# Fairness and sequential latency take the policy as a command-line flag rather than
# through the environment, so they expand here instead of through STRATEGIES.
foreach(entry IN LISTS CME_STRATEGY_MAP)
    string(REPLACE "|" ";" pair "${entry}")
    list(GET pair 0 policy)
    list(GET pair 1 infix)

    cme_case(fairness_${infix}_sym TARGET cme-fairness-test SHM_TIMEOUT 90
        ARGS --${policy} -n 8 -d 4 -i 600 --warmup 50 --bound 0.15)
    # Asymmetric: peer 0 short CS, the rest long. Best reveals ORDER starvation --
    # the ring must still give every peer its turn under skewed holds.
    cme_case(fairness_${infix}_asym TARGET cme-fairness-test SHM_TIMEOUT 90
        ARGS --${policy} -n 8 -d 4 -i 600 --warmup 50 --asymmetric --bound 0.15)
    # Uncontended sequential acquire latency. The bench prints latency; ctest only
    # checks that it runs. Run it standalone with a larger -i for real numbers.
    #
    # --trace-jsonl is passed so the dump path runs at all: without it nothing in the suite
    # reaches trace::writeJsonl or the TSC read that sizes its output, and both read as dead.
    # In a build without CME_LATENCY the write itself is an empty inline, so no file appears
    # and the only cost is the 50 ms frequency window.
    cme_case(seq_latency_${infix} TARGET cme-seq-latency-test
        ARGS --${policy} -n 8 -d 8 -i 2000 --warmup 200
             --trace-jsonl seq_latency_${infix}.jsonl)
endforeach()

# ── shm labelling sweep ─────────────────────────────────────────────────
# Anything registered without a backend label is a posix-shm case. Labelling in one
# sweep keeps `ctest -L shm` honest without a label argument at every call site, and a
# future backend that forgets its label shows up as shm rather than vanishing.
get_property(cme_all_tests DIRECTORY PROPERTY TESTS)
foreach(t IN LISTS cme_all_tests)
    get_test_property(${t} LABELS cme_test_label)
    if(NOT cme_test_label)
        set_tests_properties(${t} PROPERTIES LABELS shm)
    endif()
endforeach()

# ── Subtrees that measure rather than assert ─────────────────────────────
# Neither registers a ctest case, because a benchmark has no pass or fail. Both are build
# targets so that a header change cannot break them unnoticed. Added after the sweep
# above, which labels only the cases this directory registers.
add_subdirectory(bench)
add_subdirectory(baseline)
