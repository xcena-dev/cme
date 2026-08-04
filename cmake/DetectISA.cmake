# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# ISA feature detection, kept apart from the target lists so that the second and third check
# land beside the first rather than in the middle of a target definition.
#
# Each check answers one question: does the ISA this build already selected carry the feature?
# So the probe passes the flags cme_isa passes and adds no -m flag of its own. That
# distinction is the whole point: gcc accepts -mavx512f on any host, so a probe that passed
# -mavx512f would report what the compiler can emit rather than what this build targets, and a
# bench built on that answer would die with SIGILL instead of not existing.
#
# A feature that passes becomes an INTERFACE target, not a HAVE_ variable. A consumer then
# writes target_link_libraries(... cme_avx512) and no if() spreads through the tree. The
# target exists only where the probe passed, so linking it cannot turn on an ISA the build
# host is unable to execute.

include(CheckCXXSourceCompiles)

# The flags cme_isa puts on every TU that reaches the medium directly.
function(cme_isa_probe_flags outVar)
    set(flags -mclflushopt)
    if(CME_NATIVE_ARCH)
        list(APPEND flags -march=native)
    endif()
    string(REPLACE ";" " " joined "${flags}")
    set(${outVar} "${joined}" PARENT_SCOPE)
endfunction()

# CMAKE_REQUIRED_FLAGS needs no save and restore here. A function body has its own scope, so
# the assignment is local and the caller's value is untouched.
function(cme_check_avx512f resultVar)
    cme_isa_probe_flags(probeFlags)
    set(CMAKE_REQUIRED_FLAGS "${probeFlags}")
    check_cxx_source_compiles("
        #include <immintrin.h>
        int main()
        {
            const __m512i loaded = _mm512_loadu_si512(static_cast<const void*>(nullptr));
            return static_cast<int>(_mm512_reduce_add_epi64(loaded));
        }" ${resultVar})
endfunction()

cme_check_avx512f(CME_HAVE_AVX512F)
if(CME_HAVE_AVX512F AND NOT TARGET cme_avx512)
    add_library(cme_avx512 INTERFACE)
    target_compile_options(cme_avx512 INTERFACE -mavx512f)
endif()
