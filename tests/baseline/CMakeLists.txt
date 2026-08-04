# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# baseline/ -- the external reference point CME's numbers are read against.
#
# Nothing here is CME. rdma_lock holds no cme header and links no cme target: it is a
# two-node RDMA lock, and Section 11.3 of the design record compares CME's sweep against
# what it measures. A control belongs with the experiment, which is why it lives under
# tests/ rather than beside the library.
#
# Guarded on libibverbs, because a machine without an RDMA stack must still build the
# rest of the tree. Skipping prints a line rather than failing: absent hardware is not a
# broken checkout.

find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h)
find_library(IBVERBS_LIBRARY ibverbs)

if(NOT IBVERBS_INCLUDE_DIR OR NOT IBVERBS_LIBRARY)
    message(STATUS "baseline: libibverbs not found, skipping rdma-lock-baseline")
    return()
endif()

# A target so a header or toolchain change cannot break it unnoticed. Running it needs a
# second host, which no build can supply, so nothing here is registered with ctest.
add_executable(rdma-lock-baseline rdma_lock.cpp)
target_include_directories(rdma-lock-baseline PRIVATE ${IBVERBS_INCLUDE_DIR})
target_link_libraries(rdma-lock-baseline PRIVATE ${IBVERBS_LIBRARY} Threads::Threads)
