# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# tools/ -- cme-top, the read-only diagnostics viewer.
#
# The one thing here that installs, and the only program a user of the library runs
# directly. The sweep drivers and the THP wrapper live with the harness in tests/,
# because that is where their callers are.

add_executable(cme-top cme_top.cpp)
target_link_libraries(cme-top PRIVATE cme cme_app_warnings cme_harness)
install(TARGETS cme-top RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
