# pffft — "pretty fast FFT", the portable FFT backend for accelerate_shim.hpp.
#
# Source:  the GitHub mirror https://github.com/marton78/pffft of Julien
#          Pommier's original https://bitbucket.org/jpommier/pffft, pinned to
#          commit a4b03590cc2a4bea56f9721996e3057835799179 (mirror master as of
#          2026-07-06).
# License: BSD-3-style FFTPACK/pffft license (LICENSE.txt in the fetched
#          sources: Copyright (c) 2013 Julien Pommier / 2019 Hayati Ayguen /
#          2020 Dario Mambro, based on FFTPACKv4 Copyright (c) 2004 UCAR —
#          free for any purpose with attribution, no-endorsement, as-is).
#          GPL-free by design: FFTW was ruled out in the scoping doc
#          (docs/future/active/windows-linux-builds.md).
#
# We deliberately do NOT add_subdirectory() the upstream project (its
# CMakeLists drags in tests/benchmarks and options we don't want). Instead we
# compile the two-file C core into a small static lib of our own.
#
# This library is linked ONLY by:
#   - the portable (non-Apple) shim path, where accelerate_shim.hpp's
#     nablafx::portable_shim implementation is the real FFT backend, and
#   - test_accelerate_shim on macOS, the vDSP-vs-portable equivalence test.
# The shipped macOS plugins never link it — the mac build stays byte-identical.
include(FetchContent)

# The top-level project() is CXX-only; pffft's core is C.
enable_language(C)

FetchContent_Declare(pffft
    GIT_REPOSITORY https://github.com/marton78/pffft.git
    GIT_TAG a4b03590cc2a4bea56f9721996e3057835799179
    # No CMakeLists.txt in this subdir — FetchContent_MakeAvailable() then
    # populates the source tree WITHOUT add_subdirectory()ing upstream's build.
    SOURCE_SUBDIR cmake_subdir_intentionally_absent
)

FetchContent_MakeAvailable(pffft)

# The pinned mirror commit restructured upstream: the float-FFT core is
# src/pffft.c (which pulls the implementation from src/pffft_priv_impl.h and
# the SIMD selection from src/simd/pf_float.h) plus src/pffft_common.c
# (pffft_aligned_malloc/free). The public header lives at include/pffft/.
# We only need the single-precision real FFT — none of the double/fastconv/
# mixer extras.
#
# EXCLUDE_FROM_ALL: only built when a target that links it is built.
add_library(axon_pffft STATIC EXCLUDE_FROM_ALL
    ${pffft_SOURCE_DIR}/src/pffft.c
    ${pffft_SOURCE_DIR}/src/pffft_common.c
)
target_include_directories(axon_pffft PUBLIC ${pffft_SOURCE_DIR}/include/pffft)
set_target_properties(axon_pffft PROPERTIES POSITION_INDEPENDENT_CODE ON)
