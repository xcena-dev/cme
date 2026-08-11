# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# What libcme, cmed, the tools and both test trees all take. Header-only and INTERFACE on purpose:
# an include path is not a link dependency, so cmed keeps its rule of not linking libcme while still
# sharing these.
#
# This directory is the include root and the subdirectory is the prefix, so an include says where it
# came from: common/timing.hpp is shipped code, tests/probe_context.hpp is not.

add_library(cme_common INTERFACE)
target_include_directories(cme_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(cme-kv-config-probe tests/kv_config_probe.cpp)
target_link_libraries(cme-kv-config-probe PRIVATE cme_common cme_app_warnings)

# The file's own form rather than its values: nesting depth, a repeated key, CRLF line endings.
# Records its checks through tests/probe_context.hpp, which the daemon's probes use as well.
add_executable(cme-kv-shape-probe tests/kv_shape_probe.cpp)
target_link_libraries(cme-kv-shape-probe PRIVATE cme_common cme_app_warnings)

add_test(NAME cme_kv_config_probe COMMAND cme-kv-config-probe)
add_test(NAME cme_kv_shape_probe COMMAND cme-kv-shape-probe)
set_tests_properties(cme_kv_config_probe cme_kv_shape_probe PROPERTIES TIMEOUT 30 LABELS common)
