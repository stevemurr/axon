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

# test_mel_limiter — unit tests for the MelLimiter DSP (no external deps).
# -UNDEBUG keeps assert() live even in Release builds (these are assert-based).
add_executable(test_mel_limiter
    tests/test_mel_limiter.cpp
    src/mel_limiter.cpp
)
target_include_directories(test_mel_limiter PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_mel_limiter PRIVATE -UNDEBUG)
target_link_libraries(test_mel_limiter PRIVATE "-framework Accelerate")

# test_meter — unit tests for the LoudnessMeter (cross-checked vs LufsLeveler).
add_executable(test_meter
    tests/test_meter.cpp
    src/meter.cpp
    src/lufs_leveler.cpp
)
target_include_directories(test_meter PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_meter PRIVATE -UNDEBUG)

# test_bass_mono — unit tests for the BassMono mono-maker (header-only DSP).
add_executable(test_bass_mono tests/test_bass_mono.cpp)
target_include_directories(test_bass_mono PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_bass_mono PRIVATE -UNDEBUG)

# test_auto_gain — unit tests for the AutoGain level-match integrator.
add_executable(test_auto_gain tests/test_auto_gain.cpp)
target_include_directories(test_auto_gain PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_auto_gain PRIVATE -UNDEBUG)

# test_exciter — unit tests for the Aphex-style Exciter (header-only DSP).
add_executable(test_exciter tests/test_exciter.cpp)
target_include_directories(test_exciter PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_exciter PRIVATE -UNDEBUG)

# test_reverb — unit tests for the transparent room Reverb (header-only DSP).
add_executable(test_reverb tests/test_reverb.cpp)
target_include_directories(test_reverb PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_reverb PRIVATE -UNDEBUG)

# test_widener — unit tests for the transparent M/S stereo Widener (header-only).
add_executable(test_widener tests/test_widener.cpp)
target_include_directories(test_widener PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_widener PRIVATE -UNDEBUG)

# -----------------------------------------------------------------------------
# Regression tests added by the bug-hunt workflow. Each guards a confirmed,
# fixed bug; on the pre-fix code the matching "reject"/overflow case fails.
# -----------------------------------------------------------------------------

# test_meta — load_meta/parse_dsp_block input-validation guards (needs nlohmann).
add_executable(test_meta tests/test_meta.cpp src/meta.cpp)
target_include_directories(test_meta PRIVATE ${CMAKE_SOURCE_DIR}/src ${JSON_INCLUDE_DIR})
target_compile_options(test_meta PRIVATE -UNDEBUG)

# test_composite_meta_validate — axon_meta.json loader guards (lookahead_ms>=0).
add_executable(test_composite_meta_validate
    tests/test_composite_meta_validate.cpp
    src/composite_meta.cpp
    src/meta.cpp
)
target_include_directories(test_composite_meta_validate PRIVATE ${CMAKE_SOURCE_DIR}/src ${JSON_INCLUDE_DIR})
target_compile_options(test_composite_meta_validate PRIVATE -UNDEBUG)

# test_control_contract — the shipped axon_meta.json control set must equal the
# set of controls axon_plugin.cpp actually reads (guards composite.py drift), and
# the SSL EQ port's 21 SEQ_* controls must be present with sane specs.
add_executable(test_control_contract tests/test_control_contract.cpp)
target_include_directories(test_control_contract PRIVATE ${CMAKE_SOURCE_DIR}/src ${JSON_INCLUDE_DIR})
target_compile_options(test_control_contract PRIVATE -UNDEBUG)
target_compile_definitions(test_control_contract PRIVATE
    SRC_PATH="${CMAKE_SOURCE_DIR}/src/axon_plugin.cpp"
    META_PATH="${CMAKE_SOURCE_DIR}/../../weights/axon_bundle/axon_meta.json")

# test_nablafx_plugin — control-snapshot bounds + clamped normalization logic.
add_executable(test_nablafx_plugin tests/test_nablafx_plugin.cpp src/param_id.cpp)
target_include_directories(test_nablafx_plugin PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_nablafx_plugin PRIVATE -UNDEBUG)

# test_ort_session — run() input-length precondition (mirrors run()'s arithmetic).
add_executable(test_ort_session tests/test_ort_session.cpp)
target_include_directories(test_ort_session PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_ort_session PRIVATE -UNDEBUG)

# test_autoeq_param_guard — num_control_params <= 64 stack-buffer guard.
add_executable(test_autoeq_param_guard tests/test_autoeq_param_guard.cpp)
target_include_directories(test_autoeq_param_guard PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_autoeq_param_guard PRIVATE -UNDEBUG)
target_link_libraries(test_autoeq_param_guard PRIVATE "-framework Accelerate")

# test_true_peak — TruePeakCeiling: ceiling-never-exceeded invariant + KAT.
add_executable(test_true_peak tests/test_true_peak.cpp src/true_peak_ceiling.cpp)
target_include_directories(test_true_peak PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_true_peak PRIVATE -UNDEBUG)

# test_iir_filterbank_eq — min-phase IIR filterbank Auto-EQ renderer
# (flat=unity, magnitude-match, stability). Header-only DSP.
add_executable(test_iir_filterbank_eq tests/test_iir_filterbank_eq.cpp)
target_include_directories(test_iir_filterbank_eq PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_iir_filterbank_eq PRIVATE -UNDEBUG)

# test_multiband_exciter — narrowband HARMONICS engine: bypass, harmonics, the
# IMD-reduction property (narrow bands << one wide band), stability.
add_executable(test_multiband_exciter tests/test_multiband_exciter.cpp)
target_include_directories(test_multiband_exciter PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_multiband_exciter PRIVATE -UNDEBUG)

# test_axon_gui_js — WKWebView JS-bridge string builders (no fixed-buffer overflow).
add_executable(test_axon_gui_js tests/test_axon_gui_js.cpp)
target_include_directories(test_axon_gui_js PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_axon_gui_js PRIVATE -UNDEBUG)

# test_spectral_mask_eq — STFT mask EQ OLA output-ring overflow clamp.
add_executable(test_spectral_mask_eq tests/test_spectral_mask_eq.cpp)
target_include_directories(test_spectral_mask_eq PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_spectral_mask_eq PRIVATE -UNDEBUG)
target_link_libraries(test_spectral_mask_eq PRIVATE "-framework Accelerate")

# test_ssl_channel_eq — SSL 9000 J channel EQ engine + coupling solver
# (header-only, plain biquads — no Accelerate).
add_executable(test_ssl_channel_eq tests/test_ssl_channel_eq.cpp)
target_include_directories(test_ssl_channel_eq PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_ssl_channel_eq PRIVATE -UNDEBUG)

# test_adaptive_eq — AdaptiveEqController (productionized C1→C2 cascade auto-EQ):
# stability, adaptivity (no mode collapse), tonal direction, determinism, bypass.
add_executable(test_adaptive_eq tests/test_adaptive_eq.cpp)
target_include_directories(test_adaptive_eq PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_adaptive_eq PRIVATE -UNDEBUG)
target_link_libraries(test_adaptive_eq PRIVATE "-framework Accelerate")

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
