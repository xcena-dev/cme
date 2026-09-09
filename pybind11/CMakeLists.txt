# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# The peer session as a CPython extension, for a daemon written in Python.
#
# Off by default. It pulls in pybind11 and the Python development headers, which no other target
# here needs, and it forces PIC on libcme so the static archive can live inside a shared object.
#
# The target is cme_python and the module it writes is cme: a target named for the module would
# collide with the static library, and it is the module's name a caller types.

# Interpreter as well as the headers: pybind11's own CMake package refuses a Python3 found
# without it.
find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG QUIET)

# The distro ships headers without the CMake package on some releases, so a plain header path is
# accepted too rather than failing a build the compiler could have completed.
if(NOT pybind11_FOUND)
    find_path(PYBIND11_INCLUDE_DIR pybind11/pybind11.h)
    if(NOT PYBIND11_INCLUDE_DIR)
        message(FATAL_ERROR "CME_PYBIND is ON but pybind11 headers were not found")
    endif()
endif()

Python3_add_library(cme_python MODULE cme.cpp)

# The archive goes inside a shared object, and every translation unit in it has to be relocatable
# for that. Setting it here keeps a default build free of the flag.
set_property(TARGET cme PROPERTY POSITION_INDEPENDENT_CODE ON)

target_link_libraries(cme_python PRIVATE cme)
if(pybind11_FOUND)
    target_link_libraries(cme_python PRIVATE pybind11::headers)
else()
    target_include_directories(cme_python PRIVATE ${PYBIND11_INCLUDE_DIR})
endif()

# Hidden by default is what pybind11 expects: the module's one exported symbol is its init function,
# and a visible libcme symbol would otherwise be a second definition for whatever loads next.
set_target_properties(cme_python PROPERTIES
                      OUTPUT_NAME cme
                      CXX_VISIBILITY_PRESET hidden
                      VISIBILITY_INLINES_HIDDEN ON)
target_compile_options(cme_python PRIVATE -Wall -Wextra)

# The cases need the module on the path, which is where this target writes it. The runner comes off
# the same interpreter the module was built against, so the two cannot disagree about the ABI.
add_test(NAME cme_python
         COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR}/test_cme.py)
set_tests_properties(cme_python PROPERTIES
                     ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:cme_python>"
                     LABELS "pybind")

# Into the interpreter's own site-packages, so an importer needs no PYTHONPATH. The path comes from
# the interpreter that built the module, which is the only one whose ABI it matches.
install(TARGETS cme_python DESTINATION ${Python3_SITEARCH})
