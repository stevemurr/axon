#!/usr/bin/env bash
# run_coverage.sh — unit-test line coverage for the DSP sources.
#
# Configures a SEPARATE build dir (native/clap/build-cov) with -DAXON_COVERAGE=ON
# (LLVM source-based coverage on the test_* executables only), builds just the
# test targets, runs each test with its own LLVM_PROFILE_FILE, merges the raw
# profiles with llvm-profdata, and reports line coverage with llvm-cov
# restricted to the DSP sources: native/clap/src/*.hpp and src/*.cpp EXCLUDING
# axon_plugin.cpp and nablafx_plugin.cpp — the CLAP shells are exercised only
# through the packaged .clap, not by unit tests, so they are out of scope for
# this report (not "0%").
#
# The normal Release build dir (native/clap/build) is never touched. The
# FetchContent sources already downloaded there (_deps/*-src) are reused
# read-only when present so a fresh build-cov configure works offline and does
# not re-download onnxruntime.
#
# Usage:
#   bash native/clap/bench/run_coverage.sh
#
# Outputs (all under native/clap/build-cov/):
#   profiles/<test>.profraw   raw per-test profiles
#   coverage.profdata         merged profile
#   coverage-report.txt       llvm-cov report (also printed to stdout)
#   coverage.json             llvm-cov export -summary-only (machine-readable)
#   coverage-html/index.html  llvm-cov show HTML source annotation
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # native/clap/bench
ROOT="$(dirname "$HERE")"                              # native/clap
BUILD="$ROOT/build-cov"
NORMAL_BUILD="$ROOT/build"

# --- configure (separate dir; the normal build dir is untouched) -------------
FETCH_FLAGS=()
for dep in clap json onnxruntime; do
    src="$NORMAL_BUILD/_deps/${dep}-src"
    if [ -d "$src" ]; then
        FETCH_FLAGS+=("-DFETCHCONTENT_SOURCE_DIR_$(printf '%s' "$dep" | tr '[:lower:]' '[:upper:]')=$src")
    fi
done

cmake -S "$ROOT" -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DAXON_COVERAGE=ON \
    ${FETCH_FLAGS[@]+"${FETCH_FLAGS[@]}"}

# --- build the test targets only (not the plugin modules) --------------------
# Tests are registered via the axon_add_test() wrapper (which enforces
# -UNDEBUG + CTest registration); match plain add_executable() too for safety.
TARGETS="$(sed -nE 's/^(axon_add_test|add_executable)\((test_[A-Za-z0-9_]+).*/\2/p' "$ROOT/CMakeLists.txt" | sort -u)"
if [ -z "$TARGETS" ]; then
    echo "run_coverage.sh: no test_* targets found in $ROOT/CMakeLists.txt" >&2
    exit 1
fi
# shellcheck disable=SC2086
cmake --build "$BUILD" -j "$(sysctl -n hw.ncpu)" --target $TARGETS

# --- run every test with a per-test raw profile ------------------------------
PROFDIR="$BUILD/profiles"
rm -rf "$PROFDIR"
mkdir -p "$PROFDIR"
FAILED=0
for t in $TARGETS; do
    echo "== running $t"
    if ! (cd "$BUILD" && LLVM_PROFILE_FILE="$PROFDIR/$t.profraw" "./$t"); then
        echo "FAIL: $t" >&2
        FAILED=1
    fi
done
if [ "$FAILED" -ne 0 ]; then
    echo "run_coverage.sh: one or more tests failed — coverage report would be misleading; aborting" >&2
    exit 1
fi

xcrun llvm-profdata merge -sparse "$PROFDIR"/*.profraw -o "$BUILD/coverage.profdata"

# --- scope: DSP sources only --------------------------------------------------
# src/*.hpp + src/*.cpp minus the CLAP shells (out of scope: only exercised
# through the .clap bundle, never by unit tests).
SOURCES=()
for f in "$ROOT"/src/*.hpp "$ROOT"/src/*.cpp; do
    case "$(basename "$f")" in
        axon_plugin.cpp|nablafx_plugin.cpp) continue ;;
    esac
    SOURCES+=("$f")
done

# llvm-cov takes the first binary positionally, the rest via -object.
OBJ_ARGS=()
for t in $TARGETS; do
    if [ "${#OBJ_ARGS[@]}" -eq 0 ]; then
        OBJ_ARGS+=("$BUILD/$t")
    else
        OBJ_ARGS+=(-object "$BUILD/$t")
    fi
done

# --- report -------------------------------------------------------------------
xcrun llvm-cov report "${OBJ_ARGS[@]}" \
    -instr-profile="$BUILD/coverage.profdata" \
    "${SOURCES[@]}" | tee "$BUILD/coverage-report.txt"

xcrun llvm-cov export "${OBJ_ARGS[@]}" \
    -instr-profile="$BUILD/coverage.profdata" \
    -summary-only \
    "${SOURCES[@]}" > "$BUILD/coverage.json"

xcrun llvm-cov show "${OBJ_ARGS[@]}" \
    -instr-profile="$BUILD/coverage.profdata" \
    -format=html -output-dir="$BUILD/coverage-html" \
    "${SOURCES[@]}"

# In-scope files that never made it into any test binary have no coverage
# mapping at all — llvm-cov silently omits them, which is NOT the same as 0%.
# List them explicitly so uncovered modules can't hide.
echo ""
echo "-- in-scope sources with NO coverage mapping (not compiled into any test):"
for f in "${SOURCES[@]}"; do
    # Report lines start with the (basename'd) filename in its own column, so
    # anchor the match to avoid substring hits (meta.hpp vs composite_meta.hpp).
    if ! grep -qE "(^|/)$(basename "$f" | sed 's/\./\\./g')[[:space:]]" "$BUILD/coverage-report.txt"; then
        echo "   $(basename "$f")"
    fi
done

echo ""
echo "HTML report: $BUILD/coverage-html/index.html"
