# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# tools/ -- the programs a user of the library runs directly.
#
# The sweep drivers and the THP wrapper live with the harness in tests/, because that is
# where their callers are.
#
# cme-format takes cme_common alone, which is where the flag reader lives. cme-top still reaches
# into the harness for config_reader.hpp, and that is the next thing to move out of it: a program a
# user installs should not be linking test code.

add_executable(cme-top cme_top.cpp)
target_link_libraries(cme-top PRIVATE cme cme_app_warnings cme_harness)
install(TARGETS cme-top RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Laying out a region is one deliberate act by one operator, which is why it is not a step
# in cmed's start-up: every daemon starts at once, and Session::format does not serialise
# two callers against each other.
add_executable(cme-format cme_format.cpp)
target_link_libraries(cme-format PRIVATE cme cme_app_warnings cme_common)
install(TARGETS cme-format RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
