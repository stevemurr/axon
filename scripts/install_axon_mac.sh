#!/usr/bin/env bash
#
# Turnkey installer for the composite Axon CLAP plugin on macOS (arm64).
#
# Usage:
#   scripts/install_axon_mac.sh [--bundle DIR] [--out DIR] [--no-install]
#
# What it does:
#   1. Locates a pre-built composite staging directory containing
#      axon_meta.json + saturator/ + ssl_comp/ + auto_eq_<class>/ sub-bundle
#      directories. Lookup order:
#         1. --bundle DIR (CLI override)
#         2. ./weights/axon_bundle/   (git-tracked, populated by training host)
#         3. ./build/axon-staging/    (local rebuild output)
#         4. ./artifacts/axon-bundle/ (legacy CI drop path)
#      If none exist, errors with a hint to run scripts/export_axon.py first
#      (only meaningful on the training host where checkpoints live).
#   2. Builds the axon_clap dylib via native/clap/build.sh axon, packaging into
#      $OUT (default: build/Axon.clap).
#   3. Unless --no-install is passed, copies the .clap bundle to
#      ~/Library/Audio/Plug-Ins/CLAP/ so DAWs can pick it up.
#
# This is the only script a Mac user needs to clone-and-run after pulling the
# repo. The staging dir should be checked in or rsynced from the training host
# (it contains the ONNX models — too big for git typically; rsync from
# /shared/artifacts/exports/axon-staging/ on the GPU box is the canonical
# path).
set -eu

usage() {
    cat <<EOF >&2
usage: $(basename "$0") [--bundle <staging_dir>] [--out <out.clap>] [--no-install]

  --bundle DIR    Composite staging dir (default: ./build/axon-staging or
                  ./artifacts/axon-bundle, whichever exists).
  --out PATH      Output .clap bundle path (default: ./build/Axon.clap).
  --no-install    Build only; don't copy to ~/Library/Audio/Plug-Ins/CLAP/.
EOF
    exit 2
}

if [ "$(uname -s)" != "Darwin" ]; then
    echo "install_axon_mac.sh: must run on macOS (arm64)" >&2
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUNDLE=""
OUT="$REPO_ROOT/build/Axon.clap"
DO_INSTALL=1

while [ $# -gt 0 ]; do
    case "$1" in
        --bundle)     BUNDLE="$2"; shift 2 ;;
        --out)        OUT="$2";    shift 2 ;;
        --no-install) DO_INSTALL=0; shift ;;
        -h|--help)    usage ;;
        *)            echo "unknown arg: $1" >&2; usage ;;
    esac
done

if [ -z "$BUNDLE" ]; then
    if   [ -d "$REPO_ROOT/weights/axon_bundle"    ]; then BUNDLE="$REPO_ROOT/weights/axon_bundle"
    elif [ -d "$REPO_ROOT/build/axon-staging"     ]; then BUNDLE="$REPO_ROOT/build/axon-staging"
    elif [ -d "$REPO_ROOT/artifacts/axon-bundle"  ]; then BUNDLE="$REPO_ROOT/artifacts/axon-bundle"
    else
        cat <<EOF >&2
error: no composite staging dir found.

Expected one of:
  $REPO_ROOT/weights/axon_bundle/      (committed in this repo)
  $REPO_ROOT/build/axon-staging/       (local export output)
  $REPO_ROOT/artifacts/axon-bundle/    (legacy CI drop)

To produce one (typically on the training host, not your Mac):

  python scripts/export_axon.py from-class-dir \\
      --auto-eq-root /shared/artifacts \\
      --saturator-run /shared/artifacts/saturator_synth/.../<ts> \\
      --ssl-comp-run  /shared/artifacts/ssl_comp/.../<ts> \\
      --out           weights/axon_bundle

Then commit weights/axon_bundle/ and pull on the Mac.
EOF
        exit 1
    fi
fi

if [ ! -f "$BUNDLE/axon_meta.json" ]; then
    echo "error: $BUNDLE is missing axon_meta.json" >&2
    exit 1
fi

echo "[install_axon_mac] building Axon.clap"
echo "  staging: $BUNDLE"
echo "  out:     $OUT"

bash "$REPO_ROOT/native/clap/build.sh" axon "$BUNDLE" "$OUT"

if [ "$DO_INSTALL" -eq 1 ]; then
    # Refuse to install a bench-instrumented build (AXON_STAGE_TIMING) into the
    # DAW plugin folder — the per-stage timing instrumentation is bench-only
    # tooling. Instrumented builds must use --no-install.
    EXE_NAME="$(/bin/ls "$OUT/Contents/MacOS/" | head -1)"
    if /usr/bin/strings "$OUT/Contents/MacOS/$EXE_NAME" | grep -q "axon.stage-timing"; then
        echo "[install_axon_mac] ERROR: this build is INSTRUMENTED (AXON_STAGE_TIMING)." >&2
        echo "                   Refusing to install it into the DAW plugin folder." >&2
        echo "                   Rebuild without AXON_STAGE_TIMING=1, or pass --no-install" >&2
        echo "                   for bench builds." >&2
        exit 1
    fi

    INSTALL_DIR="$HOME/Library/Audio/Plug-Ins/CLAP"
    mkdir -p "$INSTALL_DIR"
    INSTALLED="$INSTALL_DIR/$(basename "$OUT")"

    # Clean up stale bundles from prior renames (TONE → NeuralMastering → Axon)
    # and any single-model dev installs. Hosts otherwise scan everything in
    # this directory and may load an old/broken bundle that crashes the host.
    for stale in "$INSTALL_DIR/TONE.clap" "$INSTALL_DIR/tone.clap" \
                 "$INSTALL_DIR/NeuralMastering.clap" \
                 "$INSTALL_DIR/com.nablafx.tone_"*.clap \
                 "$INSTALL_DIR/com.nablafx.neuralmastering_"*.clap; do
        if [ -e "$stale" ] && [ "$stale" != "$INSTALLED" ]; then
            echo "[install_axon_mac] removing stale bundle: $stale"
            rm -rf "$stale"
        fi
    done

    rm -rf "$INSTALLED"
    cp -R "$OUT" "$INSTALLED"
    echo "[install_axon_mac] installed to $INSTALLED"
    echo
    echo "Done. Restart your DAW (or rescan plug-ins) and look for Axon."
else
    echo "[install_axon_mac] build complete (skipped install per --no-install)"
fi
