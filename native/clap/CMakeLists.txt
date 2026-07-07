cmake_minimum_required(VERSION 3.22)

# The concrete plugin name comes from plugin_meta.json, not CMake. We build a
# single generic dylib; ``build.sh`` packages it as a per-model .clap bundle
# with the model-specific metadata.
project(nablafx_clap CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Platform seam. macOS arm64 is the reference platform: its targets get the
# EXACT historical sources/flags/links (the Windows/Linux port's guardrail is
# that mac renders stay byte-identical — verified by `uv run axon eval null`).
# Non-Apple platforms build the same targets over the portable Accelerate
# shim (src/accelerate_shim.{hpp,cpp} + pffft) and a headless GUI stub.
# See docs/future/active/windows-linux-builds.md.
#
# The two platform variables threaded through the target definitions below:
#   AXON_SHIM_SRC — extra source for every vDSP-consuming target: empty on
#                   mac (real Accelerate), src/accelerate_shim.cpp elsewhere
#                   (the pffft-backed FFT third of the shim).
#   AXON_DSP_LIBS — what those targets link: -framework Accelerate on mac,
#                   axon_pffft + libm elsewhere.
# ---------------------------------------------------------------------------
if(APPLE)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
    endif()
    set(AXON_SHIM_SRC "")
    set(AXON_DSP_LIBS "-framework Accelerate")
    set(AXON_PLATFORM_LIBS "-framework CoreFoundation")
elseif(WIN32)
    # Windows x64 headless (Phase 2 of the port). Supported toolchain is
    # clang-cl + Ninja: clang-cl keeps the compiler FRONT-END in the same
    # clang family as the macOS/Linux builds (same __builtin_* intrinsics —
    # e.g. __builtin_clzll in axon_stage_timing.h — same warning surface,
    # same C++17 conformance corners) while staying ABI-compatible with the
    # MSVC runtime that the ONNX Runtime prebuilt DLL expects.
    #   cmake -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
    # Global defines: UCRT's <cmath> only provides M_PI with
    # _USE_MATH_DEFINES; NOMINMAX/WIN32_LEAN_AND_MEAN keep <windows.h>
    # (ort_path/resource_path/dyn_module) from macro-clobbering std::min/max.
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
    set(AXON_SHIM_SRC src/accelerate_shim.cpp)
    set(AXON_DSP_LIBS axon_pffft)     # no separate libm on Windows
    set(AXON_PLATFORM_LIBS "")
else()
    set(AXON_SHIM_SRC src/accelerate_shim.cpp)
    set(AXON_DSP_LIBS axon_pffft m)
    set(AXON_PLATFORM_LIBS ${CMAKE_DL_LIBS})
endif()

include(cmake/FetchCLAP.cmake)
include(cmake/FetchOnnxRuntime.cmake)
include(cmake/FetchJson.cmake)
# pffft static lib (target axon_pffft, EXCLUDE_FROM_ALL). On mac it is linked
# ONLY by test_accelerate_shim (the shipped plugins never touch it); on other
# platforms it is the FFT backend for every shim consumer.
include(cmake/FetchPffft.cmake)

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
    ${AXON_PLATFORM_LIBS}
)

# Composite Axon plugin: separate dylib that bundles auto-EQ + saturator +
# ssl_comp behind a single AMT/TRM control surface, with the LUFS leveler and
# true-peak ceiling running in-host. spectral_mask_eq.hpp + rational_a.hpp
# are header-only so they don't appear in the source list.
#
# GUI backend selection (Phase 3 of the port). One backend file per platform
# behind the same C ABI (src/axon_gui.h) and the same JS bridge contract with
# ui/index.html (shared core: src/axon_gui_bridge.hpp):
#   macOS   — src/axon_gui.mm      (Cocoa/WKWebView; the reference impl)
#   Linux   — src/axon_gui_gtk.cpp (webkit2gtk 4.1, falling back to 4.0;
#                                    X11/XEmbed via GtkPlug)
#   Windows — src/axon_gui_win.cpp (WebView2; SDK pinned via
#                                    cmake/FetchWebView2.cmake, static loader)
# Headless fallback: src/axon_gui_stub.cpp + AXON_HAS_GUI=0 — the gui
# extension is then not offered at all and hosts show their generic param UI.
#   -DAXON_GUI=OFF      force the headless stub (also drops the webkit2gtk
#                       runtime dependency of the Linux binary)
#   -DAXON_REQUIRE_GUI=ON  configure-time error if no backend can be built
#                       (CI uses this so a silent fallback can't mask a
#                       GUI build regression)
option(AXON_GUI "Build the native GUI backend where available" ON)
option(AXON_REQUIRE_GUI "Fail configure if no native GUI backend can be built" OFF)

set(AXON_GUI_BACKEND "none")
set(AXON_GUI_LIBS "")
if(AXON_GUI AND APPLE)
    set(AXON_GUI_SRC src/axon_gui.mm)
    set(AXON_GUI_BACKEND "cocoa")
elseif(AXON_GUI AND WIN32)
    include(cmake/FetchWebView2.cmake)
    set(AXON_GUI_SRC src/axon_gui_win.cpp)
    set(AXON_GUI_BACKEND "webview2")
    # ole32: COM; user32/gdi32: the child HWND; version/advapi32/shlwapi:
    # WebView2LoaderStatic.lib dependencies.
    set(AXON_GUI_LIBS "${WEBVIEW2_LOADER_STATIC}"
        user32 gdi32 ole32 oleaut32 advapi32 shlwapi version)
elseif(AXON_GUI)  # Linux / other Unix: webkit2gtk if the dev package exists
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(AXON_WEBKITGTK QUIET IMPORTED_TARGET webkit2gtk-4.1)
        if(NOT AXON_WEBKITGTK_FOUND)
            pkg_check_modules(AXON_WEBKITGTK QUIET IMPORTED_TARGET webkit2gtk-4.0)
        endif()
        pkg_check_modules(AXON_GUI_X11 QUIET IMPORTED_TARGET x11)
    endif()
    if(AXON_WEBKITGTK_FOUND AND AXON_GUI_X11_FOUND)
        set(AXON_GUI_SRC src/axon_gui_gtk.cpp)
        set(AXON_GUI_BACKEND "webkit2gtk")
        set(AXON_GUI_LIBS PkgConfig::AXON_WEBKITGTK PkgConfig::AXON_GUI_X11)
    endif()
endif()
if(AXON_GUI_BACKEND STREQUAL "none")
    set(AXON_GUI_SRC src/axon_gui_stub.cpp)
    if(AXON_REQUIRE_GUI)
        message(FATAL_ERROR
            "AXON_REQUIRE_GUI=ON but no GUI backend is buildable here "
            "(Linux needs libwebkit2gtk-4.1-dev or -4.0-dev + libx11-dev; "
            "pass -DAXON_GUI=OFF for an intentionally headless build).")
    endif()
endif()
message(STATUS "axon_clap GUI backend: ${AXON_GUI_BACKEND}")

add_library(axon_clap MODULE
    src/axon_plugin.cpp
    ${AXON_GUI_SRC}
    src/composite_meta.cpp
    src/meta.cpp
    src/true_peak_ceiling.cpp
    src/mel_limiter.cpp
    src/meter.cpp
    src/param_id.cpp
    ${AXON_SHIM_SRC}
)

if(APPLE)
    set_source_files_properties(src/axon_gui.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
endif()

target_include_directories(axon_clap PRIVATE
    ${CLAP_INCLUDE_DIR}
    ${ONNXRUNTIME_INCLUDE_DIR}
    ${JSON_INCLUDE_DIR}
)

if(APPLE)
    target_link_libraries(axon_clap PRIVATE
        ${ONNXRUNTIME_LIBRARY}
        "-framework CoreFoundation"
        "-framework Cocoa"
        "-framework WebKit"
        "-framework Accelerate"   # vDSP_fft for SpectralMaskEq
    )
else()
    target_link_libraries(axon_clap PRIVATE
        ${ONNXRUNTIME_LIBRARY}
        ${AXON_DSP_LIBS}
        ${AXON_PLATFORM_LIBS}
        ${AXON_GUI_LIBS}
    )
endif()
if(AXON_GUI_BACKEND STREQUAL "webview2")
    target_include_directories(axon_clap PRIVATE ${WEBVIEW2_INCLUDE_DIR})
endif()

# AXON_HAS_GUI gates whether axon_plugin.cpp offers the CLAP gui extension at
# all: with the stub backend the extension is withheld so hosts go straight
# to their generic parameter UI instead of creating a dead embedded view.
if(AXON_GUI_BACKEND STREQUAL "none")
    target_compile_definitions(axon_clap PRIVATE AXON_HAS_GUI=0)
else()
    target_compile_definitions(axon_clap PRIVATE AXON_HAS_GUI=1)
endif()

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

# Install the onnxruntime dylib next to ours and make each plugin find it at
# load time: @loader_path/../Frameworks inside the mac .clap bundle; $ORIGIN
# (same directory as the plugin binary) inside the Linux directory bundle
# (see src/resource_path.hpp + package_linux.sh for the layout). Windows has
# no rpath — the DLL loader finds onnxruntime.dll next to the plugin because
# hosts load plugins with LOAD_WITH_ALTERED_SEARCH_PATH (src/dyn_module.hpp).
#
# Build-product suffix: ".so" on mac/Linux (packaging renames to the bundle
# executable), ".dll" on Windows (package_windows.py renames to <name>.clap —
# a CLAP on Windows is just a renamed DLL).
if(APPLE)
    set(AXON_PLUGIN_RPATH "@loader_path/../Frameworks")
else()
    set(AXON_PLUGIN_RPATH "$ORIGIN")
endif()
if(WIN32)
    set(AXON_PLUGIN_SUFFIX ".dll")
else()
    set(AXON_PLUGIN_SUFFIX ".so")
endif()
foreach(_t nablafx_clap axon_clap)
    set_target_properties(${_t} PROPERTIES
        PREFIX ""
        SUFFIX "${AXON_PLUGIN_SUFFIX}"
        INSTALL_RPATH "${AXON_PLUGIN_RPATH}"
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
if(NOT _have_sndfile AND WIN32)
    # No pkg-config/homebrew on Windows: pinned official win64 prebuilt.
    include(cmake/FetchSndfileWin.cmake)
    set(_have_sndfile 1)
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
    if(MSVC)  # MSVC frontend (incl. clang-cl): /U spelling of "undefine NDEBUG"
        target_compile_options(${name} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Interpreter for the python-based tests below. /usr/bin/env python3 verbatim
# on POSIX (historical behavior, kept untouched); Windows has no /usr/bin/env,
# so resolve a real interpreter at configure time.
if(WIN32)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(AXON_TEST_PYTHON ${Python3_EXECUTABLE})
else()
    set(AXON_TEST_PYTHON /usr/bin/env python3)
endif()

# test_mel_limiter — unit tests for the MelLimiter DSP (no external deps).
axon_add_test(test_mel_limiter tests/test_mel_limiter.cpp src/mel_limiter.cpp
    ${AXON_SHIM_SRC})
target_link_libraries(test_mel_limiter PRIVATE ${AXON_DSP_LIBS})

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

# test_ssl_hop_stagger — the L/R hop-phase stagger (issue #22) keeps both
# channels' TCN forwards off the same host block. Mirrors the flush cadence
# against the real kSslHop/kBlockSize; no ORT needed.
axon_add_test(test_ssl_hop_stagger tests/test_ssl_hop_stagger.cpp)

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
# Windows has no rpath: the loader resolves onnxruntime.dll starting from the
# executable's own directory, so stage the DLL next to the test binary.
if(WIN32)
    add_custom_command(TARGET test_ort_session POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_DYLIB_REAL}" "$<TARGET_FILE_DIR:test_ort_session>")
endif()

# test_autoeq_param_guard — num_control_params <= kEqParamsStorage stack-buffer
# guard (constant shared with the plugin via src/axon_limits.hpp).
axon_add_test(test_autoeq_param_guard tests/test_autoeq_param_guard.cpp
    ${AXON_SHIM_SRC})
target_link_libraries(test_autoeq_param_guard PRIVATE ${AXON_DSP_LIBS})

# test_true_peak — TruePeakCeiling: ceiling-never-exceeded invariant + KAT.
axon_add_test(test_true_peak tests/test_true_peak.cpp src/true_peak_ceiling.cpp)

# test_iir_filterbank_eq — min-phase IIR filterbank Auto-EQ renderer
# (flat=unity, magnitude-match, stability). Header-only DSP.
axon_add_test(test_iir_filterbank_eq tests/test_iir_filterbank_eq.cpp)

# test_axon_gui_js — WKWebView JS-bridge string builders (no fixed-buffer overflow).
axon_add_test(test_axon_gui_js tests/test_axon_gui_js.cpp)

# test_axon_gui_bridge — the shared GUI bridge core all three WebView backends
# sit on (axonInit golden bytes, JS->native message decoder, file:// URLs).
# Pure logic: runs on every platform's suite.
axon_add_test(test_axon_gui_bridge tests/test_axon_gui_bridge.cpp)

# test_spectral_mask_eq — STFT mask EQ OLA output-ring overflow clamp.
axon_add_test(test_spectral_mask_eq tests/test_spectral_mask_eq.cpp
    ${AXON_SHIM_SRC})
target_link_libraries(test_spectral_mask_eq PRIVATE ${AXON_DSP_LIBS})

# test_ssl_channel_eq — SSL 9000 J channel EQ engine + coupling solver
# (header-only, plain biquads — no Accelerate).
axon_add_test(test_ssl_channel_eq tests/test_ssl_channel_eq.cpp)

# test_adaptive_eq — AdaptiveEqController (productionized C1→C2 cascade auto-EQ):
# stability, adaptivity (no mode collapse), tonal direction, determinism, bypass.
axon_add_test(test_adaptive_eq tests/test_adaptive_eq.cpp ${AXON_SHIM_SRC})
target_link_libraries(test_adaptive_eq PRIVATE ${AXON_DSP_LIBS})

# test_coherence_distortion — phase-invariant distortion meter via
# magnitude-squared coherence (header-only, vDSP FFT).
axon_add_test(test_coherence_distortion tests/test_coherence_distortion.cpp
    ${AXON_SHIM_SRC})
target_link_libraries(test_coherence_distortion PRIVATE ${AXON_DSP_LIBS})

# test_accelerate_shim — the regression net for the Windows/Linux port:
# proves the portable (pffft-backed) nablafx::portable_shim implementation in
# src/accelerate_shim.{hpp,cpp} reproduces real vDSP zrip packing/scaling and
# vector-op semantics on this machine. The ONLY mac target that builds/links
# pffft — the shipped plugins never do. On non-Apple platforms the
# cross-backend sections become self-comparisons; the known-answer packing
# pins, round-trips and scalar references stay load-bearing.
axon_add_test(test_accelerate_shim
    tests/test_accelerate_shim.cpp src/accelerate_shim.cpp)
target_link_libraries(test_accelerate_shim PRIVATE
    axon_pffft ${AXON_DSP_LIBS})

# test_tolerance_stages — the per-platform acceptance oracle for the
# Windows/Linux port (scoping doc: renders CANNOT be null-tested across FFT
# backends, so each platform must prove the same steady-state physics
# instead). Runs every DSP stage on deterministic signals and asserts
# magnitude-response / level invariants within documented tolerances. Runs on
# macOS too, so a regression shows up locally, not just in platform CI.
axon_add_test(test_tolerance_stages tests/test_tolerance_stages.cpp
    src/mel_limiter.cpp src/meter.cpp ${AXON_SHIM_SRC})
target_link_libraries(test_tolerance_stages PRIVATE ${AXON_DSP_LIBS})

# test_resource_path — the cross-platform module/resource discovery seam
# (src/resource_path.hpp) + the ORTCHAR_T path-conversion boundary
# (src/ort_path.hpp). Both per-platform layout rules are plain inline
# functions compiled on EVERY platform, so the mac suite also covers the
# Linux/Windows probe logic (and vice versa); module_path_from_addr exercises
# the real per-OS API (dladdr / GetModuleFileNameW) against the test binary.
axon_add_test(test_resource_path tests/test_resource_path.cpp)
target_link_libraries(test_resource_path PRIVATE ${AXON_PLATFORM_LIBS})

# test_ssl_integration.py — plugin-level SSL EQ integration through axon_bench
# against a built build/Axon.clap. Self-skips (rc 77) when the plugin or bench
# isn't built, so ctest reports it as SKIPPED rather than failing.
add_test(NAME test_ssl_integration
         COMMAND ${AXON_TEST_PYTHON} ${CMAKE_SOURCE_DIR}/tests/test_ssl_integration.py)
set_tests_properties(test_ssl_integration PROPERTIES SKIP_RETURN_CODE 77)

# test_cli.py — unit tests for the `uv run axon` CLI router (subcommand
# routing/pass-through, the wav data-chunk comparator, the axon-run/1
# envelope, and the HTML report generator). Pure-stdlib python.
add_test(NAME test_cli
         COMMAND ${AXON_TEST_PYTHON} ${CMAKE_SOURCE_DIR}/../../axon/test_cli.py)

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
        ${AXON_PLATFORM_LIBS}
    )
    if(WIN32 AND DEFINED SNDFILE_WIN_DLL)
        # sndfile.dll next to axon_bench.exe (no rpath on Windows).
        add_custom_command(TARGET axon_bench POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SNDFILE_WIN_DLL}" "$<TARGET_FILE_DIR:axon_bench>")
    endif()
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
