# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# daemon/examples/ -- runnable programs against the requester API only.
#
# These link cmed_client and nothing from tests/, so they compile exactly as a downstream
# caller's code does. A header change that breaks the public surface breaks the build here,
# which is the point of giving them targets.
#
# Each takes the daemon's socket path as its one optional argument, so running one needs a
# daemon and nothing else.

add_executable(cmed-example-hello 01_hello_lock.cpp)
target_link_libraries(cmed-example-hello PRIVATE cmed_client cme_app_warnings)

add_executable(cmed-example-cohort 02_local_cohort.cpp)
target_link_libraries(cmed-example-cohort PRIVATE cmed_client cme_app_warnings)
