#!/usr/bin/env bash
#
# Stage a Linux .clap from an export staging directory — the Linux counterpart
# of build.sh (which is macOS-only: .clap bundles + codesign).
#
# Two modes, same CLI as build.sh:
#   single-model:   package_linux.sh <staging_dir> <out.clap>
#   composite Axon: package_linux.sh axon <staging_dir> <out.clap>
#
# LINUX RESOURCE CONVENTION (see src/resource_path.hpp for the loader side and
# docs/future/active/windows-linux-builds.md "Implementation notes"):
#
# This script produces the DIRECTORY-BUNDLE layout:
#   <OUT>/                          (a plain directory named e.g. Axon.clap)
#     <executable>.clap             (the ELF .so; rpath $ORIGIN)
#     libonnxruntime.so.<ver>       (+ SONAME symlink libonnxruntime.so.1)
#     Resources/
#       axon_meta.json + sub-bundle dirs + ui/     (composite), or
#       plugin_meta.json + model.onnx              (single-model)
#
# The plugin ALSO supports the FLAT layout (bare .clap binary with the
# resources side-by-side in the same directory) — hand-install by copying the
# bundle's contents, resources included, next to the binary. Hosts that only
# scan for regular *.clap files find the flat layout; hosts that recurse into
# directories find the bundle's inner <executable>.clap too.
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

if [ "$(uname -s)" != "Linux" ]; then
    echo "package_linux.sh: this step must run on Linux (macOS uses build.sh)" >&2
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
    # Discover all sub-bundle directory names from axon_meta.json (mirrors
    # build.sh: sub_bundles map + auto_eq classes; hardcoding would silently
    # drop newly added roles).
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
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$HERE/build"

# Configure if needed; always build so source changes are picked up.
NEED_CONFIGURE=0
[ -f "$BUILD_DIR/build_config.sh" ] || NEED_CONFIGURE=1
[ -f "$BUILD_DIR/nablafx_clap.so" ] || NEED_CONFIGURE=1
[ -f "$BUILD_DIR/axon_clap.so"    ] || NEED_CONFIGURE=1

# Release guard (mirrors build.sh): never silently ship an unoptimized build
# from a stale cache configured without CMAKE_BUILD_TYPE.
if [ -f "$BUILD_DIR/CMakeCache.txt" ] \
   && ! grep -q '^CMAKE_BUILD_TYPE:[^=]*=Release$' "$BUILD_DIR/CMakeCache.txt"; then
    NEED_CONFIGURE=1
    echo "package_linux.sh: cache CMAKE_BUILD_TYPE is not Release — forcing" \
         "reconfigure with -DCMAKE_BUILD_TYPE=Release" >&2
fi

if [ "$NEED_CONFIGURE" -eq 1 ]; then
    cmake -S "$HERE" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$BUILD_DIR" -j

# Fast contract guards before anything is packaged (mirrors build.sh).
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
exe = re.sub(r"[^a-zA-Z0-9_-]", "_", m["effect_name"]).lower().strip("_")
print(exe)
'
PYOUT=$(/usr/bin/env python3 -c "$pyscript" "$META_PATH")
MODEL_ID=$(printf '%s\n' "$PYOUT" | sed -n '1p')
EFFECT_NAME=$(printf '%s\n' "$PYOUT" | sed -n '2p')
EXECUTABLE=$(printf '%s\n' "$PYOUT" | sed -n '3p')

rm -rf "$OUT"
mkdir -p "$OUT/Resources"

cp "$DYLIB_PATH" "$OUT/$EXECUTABLE.clap"

# Post-copy sanity check (mirrors build.sh): the meta-filename string baked
# into the binary must match the mode, or the wrong dylib was staged.
EXPECTED_META=$([ "$MODE" = "axon" ] && echo "axon_meta.json" || echo "plugin_meta.json")
if ! grep -q "$EXPECTED_META" "$OUT/$EXECUTABLE.clap"; then
    echo "error: $OUT/$EXECUTABLE.clap is missing expected meta-string '$EXPECTED_META'." >&2
    echo "       This means the wrong dylib was copied for MODE=$MODE." >&2
    echo "       (Likely a stale build/; try: rm -rf $BUILD_DIR && retry)" >&2
    exit 1
fi

# ONNX Runtime next to the plugin binary ($ORIGIN rpath). The linker records
# the SONAME (libonnxruntime.so.1), so stage the real file + that symlink.
ORT_REAL_BASE="$(basename "$NABLAFX_CLAP_ORT_DYLIB")"        # libonnxruntime.so.<ver>
cp "$NABLAFX_CLAP_ORT_DYLIB" "$OUT/$ORT_REAL_BASE"
ln -sf "$ORT_REAL_BASE" "$OUT/libonnxruntime.so.1"

if [ "$MODE" = "single" ]; then
    cp "$STAGING/model.onnx"        "$OUT/Resources/"
    cp "$STAGING/plugin_meta.json"  "$OUT/Resources/"
else
    cp "$STAGING/axon_meta.json"    "$OUT/Resources/"
    for dir in $SUB_BUNDLE_DIRS; do
        cp -R "$STAGING/$dir" "$OUT/Resources/"
    done
    # Copy the WebUI so a future Linux GUI backend (Phase 3) can load it.
    cp -R "$HERE/ui" "$OUT/Resources/"
fi

echo "built $OUT"
echo "  mode:        $MODE"
echo "  effect_name: $EFFECT_NAME"
echo "  model_id:    $MODEL_ID"
echo "  executable:  $EXECUTABLE.clap"
