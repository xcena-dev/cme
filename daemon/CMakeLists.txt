# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# cmed -- one cme peer slot serving every requester on this node.
#
# shared/ is what both halves take, exported by neither: a requester never links cmed_daemon, so
# a daemon-only call fails to link instead of zeroing an area at run time. include/cmed/ is the
# only public surface; daemon/startup/config.hpp stays out of it since a requester must not set options
# for a process it does not run.

# ── What both halves take ───────────────────────────────────────────────
# The test is whether a file knows which side is calling, not what it is about. What is here is the
# POSIX wrappers and the shared-area layout on them.
add_library(cmed_shared STATIC
    shared/util/futex.cpp
    shared/util/robust_lock.cpp
    shared/posix/socket.cpp
    shared/posix/seqpacket_socket.cpp
    shared/posix/datagram_socket.cpp
    shared/area.cpp
)
# Two roots: one reaches shared/ and daemon/ by their own names, the other is what shared/
# exports, named cmed/ so a public header includes siblings by the path a consumer will use.
target_include_directories(cmed_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/shared/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# cme_common is the YAML subset the test harness reads with too, so the two cannot drift. It is
# header-only, so this is an include path and not a library cmed takes on.
target_link_libraries(cmed_shared PUBLIC Threads::Threads cme_common)
# cme/limits.hpp, for the same reason and on the same terms: the domain ceiling and the name width are
# cme's, and a requester reads them without linking cme because constants need no symbol.
target_include_directories(cmed_shared PUBLIC ${CMAKE_SOURCE_DIR}/include)

# ── The user library ────────────────────────────────────────────────────
# What a requester links. It does not link cmed_daemon, so the calls that would zero the area or eat
# a pending bit are absent at link time rather than merely undocumented.
add_library(cmed_client STATIC
    client/guard.cpp
    client/session.cpp
    client/config.cpp
)
# Two cmed/ roots and nothing else, so a requester's compile reaches only what this library means to
# show. cmed_shared is PRIVATE because its own root reaches daemon/, which would undo that at compile
# time even though the link boundary above still holds.
target_include_directories(cmed_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/client/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shared/include
)
target_link_libraries(cmed_client PUBLIC cme_common)
target_link_libraries(cmed_client PRIVATE cmed_shared)

# ── The daemon's own half ───────────────────────────────────────────────
# Test-only, and named apart from cme's so one tree can carry a build with either axis on. A case
# arms one of these with `cmed --arm <name>`, since the daemon it kills is exec'd rather than forked.
option(CMED_FAILPOINT "Compile the test-only failpoint boundaries (see daemon/observe/failpoint.hpp)" OFF)

# What a requester must not reach: formatting zeroes the area, and draining the pending bitmap
# consumes the bits a requester raised.
add_library(cmed_daemon STATIC
    daemon/observe/failpoint.cpp
    daemon/serve/workers.cpp
    daemon/serve/handler.cpp
    daemon/startup/config.cpp
    daemon/startup/served_area.cpp
    daemon/startup/service_notifier.cpp
    daemon/domain/manager.cpp
    daemon/control/handler.cpp
    daemon/control/listener.cpp
)
# The only target that links libcme, because the daemon owns the one cme::Session on this node. A
# requester never opens a cme region, so this dependency stops here.
target_link_libraries(cmed_daemon PUBLIC cmed_shared cme)
# PUBLIC, because the probes that arm a boundary compile against the same enum this half reaches.
target_compile_definitions(cmed_daemon PUBLIC $<$<BOOL:${CMED_FAILPOINT}>:CMED_FAILPOINT>)

# ── The daemon ──────────────────────────────────────────────────────────
add_executable(cmed daemon/main.cpp)
target_link_libraries(cmed PRIVATE cmed_daemon cme_app_warnings)

# Read by the config probe, and deployment artifacts otherwise, which is why they sit beside the unit
# file. One per reader. The third is the build tree's own, and sits with the tests rather than with
# what a host installs.
set(CMED_DAEMON_EXAMPLE ${CMAKE_CURRENT_SOURCE_DIR}/deploy/cmed.example.yaml)
set(CMED_CLIENT_EXAMPLE ${CMAKE_CURRENT_SOURCE_DIR}/deploy/cmed-client.example.yaml)
set(CMED_DEV_EXAMPLE ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmed-dev.yaml)

add_subdirectory(examples)
add_subdirectory(tests)
