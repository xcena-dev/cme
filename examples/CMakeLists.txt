# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# examples/ -- runnable programs against the public API only.
#
# These link cme and nothing from tests/, so they compile exactly as a downstream
# caller's code does. A header change that breaks the public surface breaks the
# build here, which is the point of giving them targets.
#
# They default to a shm URI and take one optional argument, so they run on a
# machine with no CXL hardware and need no config.yaml.

add_executable(cme-example-hello 01_hello_lock.cpp)
target_link_libraries(cme-example-hello PRIVATE cme cme_app_warnings)

add_executable(cme-example-multi-process 02_multi_process.cpp)
target_link_libraries(cme-example-multi-process PRIVATE cme cme_app_warnings)

add_executable(cme-example-threads 03_threads.cpp)
target_link_libraries(cme-example-threads PRIVATE cme cme_app_warnings Threads::Threads)

# Registered with ctest, unlike bench/: an example has a pass and a fail, and one that
# stops compiling or stops working is worse than no example. Each owns its own shm name
# and formats it on entry, so a stale region from an earlier run is not a problem.
# Labelled shm, which is the medium they default to, so `ctest -L shm` covers them.
add_test(NAME example_hello COMMAND cme-example-hello)
add_test(NAME example_multi_process COMMAND cme-example-multi-process)
add_test(NAME example_threads COMMAND cme-example-threads)

set_tests_properties(example_hello example_multi_process example_threads
                     PROPERTIES LABELS shm TIMEOUT 120)
