# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# Reads config.yaml, the per-site facts that must not be baked into the build
# files: which devdax device this machine has, where marufs is mounted, which
# RoCE HCA the sweep scripts talk to. None of it is true on another machine.
#
# The file is deliberately restricted to flat `key: value` lines. That subset
# is valid YAML, and it is also what CMake and a shell script can each parse
# on their own, so neither consumer needs a parser dependency.
#
# cme_site_config_load(<path>) defines CME_SITE_<key> for every key found, and
# sets CME_SITE_FOUND. A missing file is not an error: every consumer falls
# back to a value that disables the feature rather than to one machine's path.

function(cme_site_config_load path)
    if(NOT EXISTS "${path}")
        set(CME_SITE_FOUND FALSE PARENT_SCOPE)
        return()
    endif()
    set(CME_SITE_FOUND TRUE PARENT_SCOPE)

    # file(READ) then split by hand. file(STRINGS) behaves like strings(1) and
    # treats any non-ASCII byte as a separator, so a box-drawing character in a
    # comment would arrive as its own bogus line.
    file(READ "${path}" content)
    string(REPLACE ";" "\\;" content "${content}")
    string(REPLACE "\r" "" content "${content}")
    string(REPLACE "\n" ";" content "${content}")

    foreach(line IN LISTS content)
        if(line MATCHES "^[ \t]*$" OR line MATCHES "^[ \t]*#")
            continue()
        endif()
        if(NOT line MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*:[ \t]*(.*)$")
            message(FATAL_ERROR
                "${path}: only flat 'key: value' lines are supported, got: ${line}")
        endif()
        set(key "${CMAKE_MATCH_1}")
        set(value "${CMAKE_MATCH_2}")
        string(REGEX REPLACE "[ \t]+#.*$" "" value "${value}")   # trailing comment
        string(STRIP "${value}" value)
        string(REGEX REPLACE "^\"(.*)\"$" "\\1" value "${value}")
        string(REGEX REPLACE "^'(.*)'$" "\\1" value "${value}")
        set(CME_SITE_${key} "${value}" PARENT_SCOPE)
    endforeach()
endfunction()
