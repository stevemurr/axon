cmake_minimum_required(VERSION 3.22)

# The concrete plugin name comes from plugin_meta.json, not CMake. We build a
# single generic dylib; ``build.sh`` packages it as a per-model .clap bundle
# with the model-specific metadata.
project(nablafx_clap CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT APPLE)
    message(FATAL_ERROR "v1 targets macOS arm64 only. Cross/universal builds are v2.")
endif()

if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()

include(cmake/FetchCLAP.cmake)
include(cmake/FetchOnnxRuntime.cmake)
include(cmake/FetchJson.cmake)

add_library(nablafx_clap MODULE
    src/nablafx_plugin.cpp
    src/meta.cpp
    src/ort_session.cpp
    src/param_id.cpp
)

target_include_directories(nablafx_clap PRIVATE
    ${CLAP_INCLUDE_DIR}
    ${ONNXRUNTIME_INCLUDE_DIR}
    ${JSON_INCLUDE_DIR}
)

target_link_libraries(nablafx_clap PRIVATE
    ${ONNXRUNTIME_LIBRARY}
    "-framework CoreFoundation"
)

# Composite Axon plugin: separate dylib that bundles auto-EQ + saturator +
# ssl_comp behind a single AMT/TRM control surface, with the LUFS leveler and
# true-peak ceiling running in-host. spectral_mask_eq.hpp + rational_a.hpp
# are header-only so they don't appear in the source list.
add_library(axon_clap MODULE
    src/axon_plugin.cpp
    src/axon_gui.mm
    src/composite_meta.cpp
    src/meta.cpp
    src/true_peak_ceiling.cpp
    src/mel_limiter.cpp
    src/meter.cpp
    src/param_id.cpp
)

set_source_files_properties(src/axon_gui.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc"
)

target_include_directories(axon_clap PRIVATE
    ${CLAP_INCLUDE_DIR}
    ${ONNXRUNTIME_INCLUDE_DIR}
    ${JSON_INCLUDE_DIR}
)

target_link_libraries(axon_clap PRIVATE
    ${ONNXRUNTIME_LIBRARY}
    "-framework CoreFoundation"
    "-framework Cocoa"
    "-framework WebKit"
    "-framework Accelerate"   # vDSP_fft for SpectralMaskEq
)

# Per-stage CPU timing instrumentation (src/axon_stage_timing.h). Bench-only:
# adds two clock reads around every stage in the audio hot path and exposes a
# custom CLAP extension for readout. OFF by default — default builds are
# bit-identical to uninstrumented ones. build.sh has a stale-cache guard so an
# instrumented cache can never silently ship.
option(AXON_STAGE_TIMING "Per-stage timing instrumentation (bench-only; do NOT ship)" OFF)
if(AXON_STAGE_TIMING)
    target_compile_definitions(axon_clap PRIVATE AXON_STAGE_TIMING=1)
    message(STATUS "axon_clap: AXON_STAGE_TIMING=ON (instrumented; bench-only, do NOT ship)")
endif()

# Install the onnxruntime dylib next to ours and make each plugin find it
# via @loader_path/../Frameworks once it's inside its .clap bundle.
foreach(_t nablafx_clap axon_clap)
    set_target_properties(${_t} PROPERTIES
        PREFIX ""
        SUFFIX ".so"                 # build.sh renames to the bundle executable
        INSTALL_RPATH "@loader_path/../Frameworks"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endforeach()

# Expose artifacts to build.sh via a tiny config file
configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/build_config.cmake.in
    ${CMAKE_BINARY_DIR}/build_config.sh
    @ONLY
)

# -----------------------------------------------------------------------------
# axon_bench — headless CLAP host for benchmarking the composite plugin.
# Optional: only built if libsndfile is found (homebrew: `brew install libsndfile`).
# -----------------------------------------------------------------------------
find_package(PkgConfig QUIET)
set(_have_sndfile 0)
if(PkgConfig_FOUND)
    pkg_check_modules(SNDFILE QUIET sndfile)
    if(SNDFILE_FOUND)
        set(_have_sndfile 1)
    endif()
endif()
if(NOT _have_sndfile)
    foreach(_p /opt/homebrew /usr/local)
        if(EXISTS "${_p}/include/sndfile.h"
           AND (EXISTS "${_p}/lib/libsndfile.dylib" OR EXISTS "${_p}/lib/libsndfile.a"))
            set(SNDFILE_INCLUDE_DIRS "${_p}/include")
            set(SNDFILE_LIBRARY_DIRS "${_p}/lib")
            set(SNDFILE_LIBRARIES sndfile)
            set(_have_sndfile 1)
            break()
        endif()
    endforeach()
endif()

# -----------------------------------------------------------------------------
# Test targets. axon_add_test() enforces two suite-wide invariants that used to
# be hand-repeated per target:
#   - -UNDEBUG on every test: the tests are assert-based, and Release's
#     -DNDEBUG would silently turn a target added without the flag into a
#     vacuous no-op.
#   - CTest registration: the canonical suite command is
#         ctest --test-dir native/clap/build --output-on-failure
#     which only runs registered, freshly-built targets (a raw `for t in
#     test_*` glob over the build dir happily keeps running a stale binary
#     whose target was deleted or renamed).
# -----------------------------------------------------------------------------
enable_testing()

function(axon_add_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${name} PRIVATE -UNDEBUG)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# test_mel_limiter — unit tests for the MelLimiter DSP (no external deps).
axon_add_test(test_mel_limiter tests/test_mel_limiter.cpp src/mel_limiter.cpp)
target_link_libraries(test_mel_limiter PRIVATE "-framework Accelerate")

# test_meter — unit tests for the LoudnessMeter (cross-checked vs LufsLeveler).
axon_add_test(test_meter tests/test_meter.cpp src/meter.cpp src/lufs_leveler.cpp)

# test_bass_mono — unit tests for the BassMono mono-maker (header-only DSP).
axon_add_test(test_bass_mono tests/test_bass_mono.cpp)

# test_auto_gain — unit tests for the AutoGain level-match integrator.
axon_add_test(test_auto_gain tests/test_auto_gain.cpp)

# test_reverb — unit tests for the transparent room Reverb (header-only DSP).
axon_add_test(test_reverb tests/test_reverb.cpp)

# test_widener — unit tests for the transparent M/S stereo Widener (header-only).
axon_add_test(test_widener tests/test_widener.cpp)

# test_dsp — LufsLeveler + TruePeakCeiling transparency/limit tests (the only
# LufsLeveler-focused coverage).
axon_add_test(test_dsp tests/test_dsp.cpp src/lufs_leveler.cpp src/true_peak_ceiling.cpp)

# test_grey_dsp — RationalA grey-box DSP eval + meta plumbing.
axon_add_test(test_grey_dsp tests/test_grey_dsp.cpp src/meta.cpp)
target_include_directories(test_grey_dsp PRIVATE ${JSON_INCLUDE_DIR})

# test_composite_meta — composite axon_meta.json loader sanity, run against the
# committed weights/axon_bundle (DEFAULT_META_PATH; argv/env can override).
axon_add_test(test_composite_meta
    tests/test_composite_meta.cpp src/composite_meta.cpp src/meta.cpp)
target_include_directories(test_composite_meta PRIVATE ${JSON_INCLUDE_DIR})
target_compile_definitions(test_composite_meta PRIVATE
    DEFAULT_META_PATH="${CMAKE_SOURCE_DIR}/../../weights/axon_bundle/axon_meta.json")

# -----------------------------------------------------------------------------
# Regression tests added by the bug-hunt workflow. Each guards a confirmed,
# fixed bug; on the pre-fix code the matching "reject"/overflow case fails.
# -----------------------------------------------------------------------------

# test_meta — load_meta/parse_dsp_block input-validation guards (needs nlohmann).
axon_add_test(test_meta tests/test_meta.cpp src/meta.cpp)
target_include_directories(test_meta PRIVATE ${JSON_INCLUDE_DIR})

# test_composite_meta_validate — axon_meta.json loader guards (lookahead_ms>=0).
axon_add_test(test_composite_meta_validate
    tests/test_composite_meta_validate.cpp src/composite_meta.cpp src/meta.cpp)
target_include_directories(test_composite_meta_validate PRIVATE ${JSON_INCLUDE_DIR})

# test_control_contract — the shipped axon_meta.json control set must equal the
# set of controls axon_plugin.cpp actually reads (guards composite.py drift), and
# the SSL EQ port's 22 SEQ_* controls must be present with sane specs.
axon_add_test(test_control_contract tests/test_control_contract.cpp)
target_include_directories(test_control_contract PRIVATE ${JSON_INCLUDE_DIR})
target_compile_definitions(test_control_contract PRIVATE
    SRC_PATH="${CMAKE_SOURCE_DIR}/src/axon_plugin.cpp"
    META_PATH="${CMAKE_SOURCE_DIR}/../../weights/axon_bundle/axon_meta.json")

# test_ssl_hop_contract — kSslHop (axon_limits.hpp) must satisfy
# kSslHop <= trace_len - receptive_field against the SHIPPED ssl_comp bundle
# (margin is exactly 0 today; a re-export that shrinks it fails here, not in
# the DAW at activate).
axon_add_test(test_ssl_hop_contract tests/test_ssl_hop_contract.cpp src/meta.cpp)
target_include_directories(test_ssl_hop_contract PRIVATE ${JSON_INCLUDE_DIR})
target_compile_definitions(test_ssl_hop_contract PRIVATE
    SSL_META_PATH="${CMAKE_SOURCE_DIR}/../../weights/axon_bundle/ssl_comp/plugin_meta.json")

# test_nablafx_plugin — control-snapshot bounds + clamped normalization logic.
axon_add_test(test_nablafx_plugin tests/test_nablafx_plugin.cpp src/param_id.cpp)

# test_ort_session — run() input-length precondition (calls the real
# ort_run_out_len from src/ort_run_guard.hpp, the function run() itself uses)
# + the REAL OrtSession (src/ort_session.cpp) run against tiny deterministic
# ONNX graphs embedded in the test, so it links the actual onnxruntime dylib.
axon_add_test(test_ort_session tests/test_ort_session.cpp src/ort_session.cpp)
target_include_directories(test_ort_session PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
target_link_libraries(test_ort_session PRIVATE ${ONNXRUNTIME_LIBRARY})
# The prebuilt dylib's install name is @rpath/libonnxruntime.<ver>.dylib; the
# test runs from the build dir, so point rpath at the FetchContent lib dir.
set_target_properties(test_ort_session PROPERTIES
    BUILD_RPATH "${ONNXRUNTIME_ROOT}/lib")

# test_autoeq_param_guard — num_control_params <= kEqParamsStorage stack-buffer
# guard (constant shared with the plugin via src/axon_limits.hpp).
axon_add_test(test_autoeq_param_guard tests/test_autoeq_param_guard.cpp)
target_link_libraries(test_autoeq_param_guard PRIVATE "-framework Accelerate")

# test_true_peak — TruePeakCeiling: ceiling-never-exceeded invariant + KAT.
axon_add_test(test_true_peak tests/test_true_peak.cpp src/true_peak_ceiling.cpp)

# test_iir_filterbank_eq — min-phase IIR filterbank Auto-EQ renderer
# (flat=unity, magnitude-match, stability). Header-only DSP.
axon_add_test(test_iir_filterbank_eq tests/test_iir_filterbank_eq.cpp)

# test_axon_gui_js — WKWebView JS-bridge string builders (no fixed-buffer overflow).
axon_add_test(test_axon_gui_js tests/test_axon_gui_js.cpp)

# test_spectral_mask_eq — STFT mask EQ OLA output-ring overflow clamp.
axon_add_test(test_spectral_mask_eq tests/test_spectral_mask_eq.cpp)
target_link_libraries(test_spectral_mask_eq PRIVATE "-framework Accelerate")

# test_ssl_channel_eq — SSL 9000 J channel EQ engine + coupling solver
# (header-only, plain biquads — no Accelerate).
axon_add_test(test_ssl_channel_eq tests/test_ssl_channel_eq.cpp)

# test_adaptive_eq — AdaptiveEqController (productionized C1→C2 cascade auto-EQ):
# stability, adaptivity (no mode collapse), tonal direction, determinism, bypass.
axon_add_test(test_adaptive_eq tests/test_adaptive_eq.cpp)
target_link_libraries(test_adaptive_eq PRIVATE "-framework Accelerate")

# test_coherence_distortion — phase-invariant distortion meter via
# magnitude-squared coherence (header-only, vDSP FFT).
axon_add_test(test_coherence_distortion tests/test_coherence_distortion.cpp)
target_link_libraries(test_coherence_distortion PRIVATE "-framework Accelerate")

# test_ssl_integration.py — plugin-level SSL EQ integration through axon_bench
# against a built build/Axon.clap. Self-skips (rc 77) when the plugin or bench
# isn't built, so ctest reports it as SKIPPED rather than failing.
add_test(NAME test_ssl_integration
         COMMAND /usr/bin/env python3 ${CMAKE_SOURCE_DIR}/tests/test_ssl_integration.py)
set_tests_properties(test_ssl_integration PROPERTIES SKIP_RETURN_CODE 77)

# test_cli.py — unit tests for the `uv run axon` CLI router (subcommand
# routing/pass-through, the wav data-chunk comparator, the axon-run/1
# envelope, and the HTML report generator). Pure-stdlib python.
add_test(NAME test_cli
         COMMAND /usr/bin/env python3 ${CMAKE_SOURCE_DIR}/../../axon/test_cli.py)

if(_have_sndfile)
    add_executable(axon_bench bench/axon_bench.cpp)
    target_include_directories(axon_bench PRIVATE
        ${CLAP_INCLUDE_DIR}
        ${SNDFILE_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/src      # axon_stage_timing.h (timing readout ABI)
    )
    target_link_directories(axon_bench PRIVATE ${SNDFILE_LIBRARY_DIRS})
    target_link_libraries(axon_bench PRIVATE
        ${SNDFILE_LIBRARIES}
        "-framework CoreFoundation"
    )
    message(STATUS "axon_bench: enabled (sndfile in ${SNDFILE_INCLUDE_DIRS})")
else()
    message(STATUS "axon_bench: SKIPPED (libsndfile not found; brew install libsndfile)")
endif()

# -----------------------------------------------------------------------------
# AXON_COVERAGE — LLVM source-based coverage for the unit tests ONLY.
# Instruments every test_* executable (compile AND link) with
# -fprofile-instr-generate -fcoverage-mapping. The plugin modules
# (nablafx_clap/axon_clap) and axon_bench are deliberately NOT instrumented:
# shipped dylibs must stay bit-identical to uninstrumented builds.
# Intended to be used in a separate build dir via bench/run_coverage.sh so the
# normal Release build dir is never polluted with an instrumented cache.
# -----------------------------------------------------------------------------
option(AXON_COVERAGE "Coverage instrumentation for test_* executables (use bench/run_coverage.sh; do NOT enable in the normal build dir)" OFF)
if(AXON_COVERAGE)
    get_property(_axon_all_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
    set(_axon_cov_targets "")
    foreach(_t IN LISTS _axon_all_targets)
        if(_t MATCHES "^test_")
            target_compile_options(${_t} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
            target_link_options(${_t} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
            list(APPEND _axon_cov_targets ${_t})
        endif()
    endforeach()
    message(STATUS "AXON_COVERAGE=ON: instrumented test targets: ${_axon_cov_targets}")
endif()
