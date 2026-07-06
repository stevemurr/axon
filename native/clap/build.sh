#!/usr/bin/env bash
#
# Build a .clap bundle from an export staging directory.
#
# Two modes:
#   single-model:   build.sh <staging_dir> <out.clap>
#                   stages a per-model .clap from a `nablafx-export` bundle
#                   (model.onnx + plugin_meta.json).
#
#   composite Axon: build.sh axon <staging_dir> <out.clap>
#                   stages the composite Axon plugin from a
#                   `scripts/export_axon.py` bundle (axon_meta.json +
#                   sub-bundle dirs: auto_eq/, saturator/, ssl_comp/).
#
# Behavior:
#   - On first run (or after cmake inputs change) configures and builds the
#     dylibs under native/clap/build/.
#   - Stages the appropriate dylib + the onnxruntime dylib + model bundle
#     resources into the .clap bundle with the standard Mac layout.
#   - Ad-hoc codesigns the bundle (`codesign --force --deep --sign -`). Local
#     dev only; distribution needs a real identity + notarize.
set -eu

usage() {
    cat <<EOF >&2
usage:
  $(basename "$0") <staging_dir> <out.clap>          # single-model
  $(basename "$0") axon <staging_dir> <out.clap>     # composite Axon
EOF
    exit 2
}

MODE="single"
if [ "${1:-}" = "axon" ]; then
    MODE="axon"
    shift
fi

if [ "$#" -ne 2 ]; then
    usage
fi

if [ "$(uname -s)" != "Darwin" ]; then
    echo "build.sh: this step must run on macOS (arm64)" >&2
    exit 1
fi

STAGING="$(cd "$1" && pwd)"
OUT="$2"

if [ "$MODE" = "single" ]; then
    if [ ! -f "$STAGING/model.onnx" ] || [ ! -f "$STAGING/plugin_meta.json" ]; then
        echo "error: $STAGING is missing model.onnx or plugin_meta.json" >&2
        exit 1
    fi
else
    if [ ! -f "$STAGING/axon_meta.json" ]; then
        echo "error: $STAGING is missing axon_meta.json" >&2
        exit 1
    fi
    # Discover all sub-bundle directory names from axon_meta.json
    # (sub_bundles map for single-instance stages + auto_eq.classes for the
    # multi-class auto-EQ). Hardcoding the list here would silently drop any
    # new role added later (e.g. how ssl_comp got missed when first added).
    SUB_BUNDLE_DIRS=$(/usr/bin/env python3 - "$STAGING/axon_meta.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
dirs = set()
for d in (m.get("sub_bundles") or {}).values():
    dirs.add(d)
for d in (m.get("auto_eq", {}).get("classes") or {}).values():
    dirs.add(d)
print(" ".join(sorted(dirs)))
PY
)
    if [ -z "${SUB_BUNDLE_DIRS}" ]; then
        echo "error: axon_meta.json declares no sub_bundles or auto_eq.classes" >&2
        exit 1
    fi
    for dir in $SUB_BUNDLE_DIRS; do
        if [ ! -d "$STAGING/$dir" ]; then
            echo "error: $STAGING is missing sub-bundle $dir/ (declared in axon_meta.json)" >&2
            exit 1
        fi
    done
    # AUTOEQ_DIRS kept for symmetry with the copy step below.
    AUTOEQ_DIRS=$(/usr/bin/env python3 - "$STAGING/axon_meta.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
classes = m.get("auto_eq", {}).get("classes") or {}
print(" ".join(sorted(set(classes.values()))))
PY
)
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$HERE/build"

# Configure if needed; always build so source changes are picked up.
NEED_CONFIGURE=0
[ -f "$BUILD_DIR/build_config.sh" ] || NEED_CONFIGURE=1
[ -f "$BUILD_DIR/nablafx_clap.so" ] || NEED_CONFIGURE=1
[ -f "$BUILD_DIR/axon_clap.so"    ] || NEED_CONFIGURE=1

# --- AXON_STAGE_TIMING plumbing (bench-only instrumentation; never ship) ----
# Env var AXON_STAGE_TIMING=1 forces a reconfigure with the timing option ON.
# Stale-cache guard: configure is normally SKIPPED when build outputs exist,
# so if a previous instrumented configure left AXON_STAGE_TIMING:BOOL=ON in
# the cache and the env var is NOT set now, we must force it back OFF — an
# instrumented plugin must never silently ship from a stale cache.
TIMING_FLAG=""
CACHE_TIMING_ON=0
if [ -f "$BUILD_DIR/CMakeCache.txt" ] \
   && grep -q '^AXON_STAGE_TIMING:BOOL=ON$' "$BUILD_DIR/CMakeCache.txt"; then
    CACHE_TIMING_ON=1
fi
if [ "${AXON_STAGE_TIMING:-0}" = "1" ]; then
    NEED_CONFIGURE=1
    TIMING_FLAG="-DAXON_STAGE_TIMING=ON"
    echo "build.sh: AXON_STAGE_TIMING=1 — building INSTRUMENTED plugin (bench-only; do NOT ship)" >&2
elif [ "$CACHE_TIMING_ON" -eq 1 ]; then
    NEED_CONFIGURE=1
    TIMING_FLAG="-DAXON_STAGE_TIMING=OFF"
    cat >&2 <<'EOF'
############################################################################
# build.sh: STALE INSTRUMENTED CACHE DETECTED                              #
#                                                                          #
# The existing CMake cache has AXON_STAGE_TIMING:BOOL=ON (a previous       #
# bench-instrumented build) but AXON_STAGE_TIMING=1 is not set in the      #
# environment for this run. Forcing a reconfigure with the option OFF so   #
# the per-stage timing instrumentation cannot silently ship.               #
#                                                                          #
# To build instrumented on purpose:  AXON_STAGE_TIMING=1 ./build.sh ...    #
############################################################################
EOF
fi
# ----------------------------------------------------------------------------

# --- Release guard: a stale cache configured without CMAKE_BUILD_TYPE builds
# with NO optimization and silently ships a ~3x slower plugin (this happened:
# an -O0 build reached the DAW on 2026-07-05). Configure is skipped when build
# outputs exist, so check the cache's build type and force a reconfigure (the
# configure below always passes -DCMAKE_BUILD_TYPE=Release) when it is
# anything but Release.
if [ -f "$BUILD_DIR/CMakeCache.txt" ] \
   && ! grep -q '^CMAKE_BUILD_TYPE:[^=]*=Release$' "$BUILD_DIR/CMakeCache.txt"; then
    NEED_CONFIGURE=1
    echo "build.sh: cache CMAKE_BUILD_TYPE is not Release — forcing reconfigure" \
         "with -DCMAKE_BUILD_TYPE=Release (unoptimized builds must never ship)" >&2
fi

if [ "$NEED_CONFIGURE" -eq 1 ]; then
    cmake -S "$HERE" -B "$BUILD_DIR" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release ${TIMING_FLAG:+"$TIMING_FLAG"}
fi
cmake --build "$BUILD_DIR" -j

# Fast contract guards (<1s total). The meta<->C++ control contract and the
# composite-meta loader guards must hold BEFORE anything is packaged: a
# re-exported bundle with a drifted control set would otherwise build,
# codesign and install cleanly, and only fail if someone remembers to run the
# test suite by hand. set -eu aborts the build on any failure.
"$BUILD_DIR/test_control_contract"
"$BUILD_DIR/test_composite_meta_validate"

# shellcheck disable=SC1091
. "$BUILD_DIR/build_config.sh"

if [ "$MODE" = "single" ]; then
    META_PATH="$STAGING/plugin_meta.json"
    DYLIB_PATH="$NABLAFX_CLAP_DYLIB"
else
    META_PATH="$STAGING/axon_meta.json"
    DYLIB_PATH="$AXON_CLAP_DYLIB"
fi

pyscript='
import json, re, sys
m = json.load(open(sys.argv[1]))
print(m["model_id"])
print(m["effect_name"])
# Sanitize effect_name for use as a filename (model_id can exceed 255-char OS limit)
exe = re.sub(r"[^a-zA-Z0-9_-]", "_", m["effect_name"]).lower().strip("_")
print(exe)
'
PYOUT=$(/usr/bin/env python3 -c "$pyscript" "$META_PATH")
MODEL_ID=$(printf '%s\n' "$PYOUT" | sed -n '1p')
EFFECT_NAME=$(printf '%s\n' "$PYOUT" | sed -n '2p')
EXECUTABLE=$(printf '%s\n' "$PYOUT" | sed -n '3p')

# Layout:
#   <OUT>/Contents/Info.plist
#   <OUT>/Contents/MacOS/<executable>
#   <OUT>/Contents/Frameworks/libonnxruntime.<ver>.dylib
#   <OUT>/Contents/Frameworks/libonnxruntime.dylib (symlink)
#   <OUT>/Contents/Resources/...      (model.onnx + plugin_meta.json for single,
#                                       axon_meta.json + sub-bundle dirs for axon)
rm -rf "$OUT"
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Frameworks" "$OUT/Contents/Resources"

cp "$DYLIB_PATH"             "$OUT/Contents/MacOS/$EXECUTABLE"
cp "$NABLAFX_CLAP_ORT_DYLIB" "$OUT/Contents/Frameworks/"

# Post-copy sanity check: verify the dylib is the right binary for this
# mode by looking at the meta-filename string baked into the .so. Catches
# the failure mode where a stale build/ dir or wrong cmake target produced
# a dylib that loads the wrong meta file (would crash at host load with no
# obvious diagnostic).
COPIED_DYLIB="$OUT/Contents/MacOS/$EXECUTABLE"
EXPECTED_META=$([ "$MODE" = "axon" ] && echo "axon_meta.json" || echo "plugin_meta.json")
if ! /usr/bin/strings "$COPIED_DYLIB" | grep -q "$EXPECTED_META"; then
    echo "error: $COPIED_DYLIB is missing expected meta-string '$EXPECTED_META'." >&2
    echo "       This means the wrong dylib was copied for MODE=$MODE." >&2
    echo "       (Likely a stale build/; try: rm -rf $BUILD_DIR && retry)" >&2
    exit 1
fi
# NOTE: no "wrong-mode string present" check — axon_clap.so legitimately
# contains BOTH meta-filename strings (it loads sub-bundle plugin_meta.json
# files), so only the positive check above carries signal.
ln -sf "$(basename "$NABLAFX_CLAP_ORT_DYLIB")" \
       "$OUT/Contents/Frameworks/libonnxruntime.dylib"

if [ "$MODE" = "single" ]; then
    cp "$STAGING/model.onnx"        "$OUT/Contents/Resources/"
    cp "$STAGING/plugin_meta.json"  "$OUT/Contents/Resources/"
else
    cp "$STAGING/axon_meta.json"    "$OUT/Contents/Resources/"
    # SUB_BUNDLE_DIRS already covers single-instance roles + auto_eq classes
    # (see the validation block above). Copy them all in one pass.
    for dir in $SUB_BUNDLE_DIRS; do
        cp -R "$STAGING/$dir" "$OUT/Contents/Resources/"
    done
    # Copy the WebUI so the plugin can load it at runtime.
    cp -R "$HERE/ui" "$OUT/Contents/Resources/"
fi

BUNDLE_ID="com.nablafx.$EXECUTABLE"
sed \
    -e "s|__BUNDLE_EXECUTABLE__|$EXECUTABLE|g" \
    -e "s|__BUNDLE_IDENTIFIER__|$BUNDLE_ID|g" \
    -e "s|__BUNDLE_NAME__|$EFFECT_NAME|g" \
    "$HERE/template/Info.plist.in" > "$OUT/Contents/Info.plist"

# Ad-hoc codesign for local dev. Distribution builds need a real identity.
codesign --force --deep --sign - "$OUT"

echo "built $OUT"
echo "  mode:        $MODE"
echo "  effect_name: $EFFECT_NAME"
echo "  model_id:    $MODEL_ID"
echo "  bundle_id:   $BUNDLE_ID"
