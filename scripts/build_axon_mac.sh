#!/usr/bin/env bash
#
# Mac one-shot: bundles the in-repo Axon artifacts
# (artifacts/axon-bundles/*) into Axon.clap and (optionally)
# installs it into the system CLAP plugin directory.
#
# Usage:
#   bash scripts/build_axon_mac.sh                 # writes ./build/Axon.clap
#   bash scripts/build_axon_mac.sh install         # also copies to ~/Library/Audio/Plug-Ins/CLAP/
#   bash scripts/build_axon_mac.sh <out.clap>      # write to a custom path
#
# Prerequisites on the Mac:
#   - macOS arm64 + Xcode Command Line Tools (`xcode-select --install`)
#   - cmake (`brew install cmake`)
#   - python3 ≥ 3.10 (ships with CLT — only used to read JSON in build.sh)
#
# This script does NOT need uv / the nablafx Python package: it consumes the
# already-staged ONNX bundles in artifacts/axon-bundles/ and the C++ source
# under native/clap/. Use scripts/export_axon.py to re-stage if you retrain.
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUNDLES="$REPO/artifacts/axon-bundles"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "build_axon_mac.sh: must run on macOS (the .clap dylib is mach-o)" >&2
    exit 1
fi

# Resolve output path / install behavior.
INSTALL=0
OUT=""
case "${1:-}" in
    "")        OUT="$REPO/build/Axon.clap" ;;
    install)   OUT="$REPO/build/Axon.clap"; INSTALL=1 ;;
    *)         OUT="$1" ;;
esac

# Sanity-check the staged bundles.
for sub in auto_eq saturator ssl_comp; do
    if [ ! -d "$BUNDLES/$sub" ]; then
        echo "error: missing $BUNDLES/$sub — committed bundles disappeared?" >&2
        exit 1
    fi
done

# Compose the staging dir under build/ (ephemeral, regenerated each run).
STAGING="$REPO/build/axon-staging"
rm -rf "$STAGING"
mkdir -p "$STAGING"

# We don't depend on uv being installed; use the host python3 to compose the
# axon_meta.json. The composition is a few file copies + a JSON template, so
# inline it here to avoid spinning up uv.
/usr/bin/env python3 - <<EOF
import json, shutil
from pathlib import Path
import sys

repo = Path("$REPO")
bundles = repo / "artifacts" / "axon-bundles"
out_dir = Path("$STAGING")

# Load composite.py directly to avoid pulling in nablafx (which has heavy
# training-only deps not needed by build_mac). composite.py only uses stdlib
# + json, so a bare importlib load is enough.
import importlib.util, sys as _sys
_spec = importlib.util.spec_from_file_location(
    "composite",
    repo / "axon" / "export" / "composite.py",
)
_mod = importlib.util.module_from_spec(_spec)
_sys.modules["composite"] = _mod   # must be registered before exec so dataclass __module__ resolves (Python 3.14+)
_spec.loader.exec_module(_mod)
export_composite_bundle = _mod.export_composite_bundle

auto_eq_root = bundles / "auto_eq"
auto_eq_bundles = {d.name: d for d in auto_eq_root.iterdir() if d.is_dir()}

meta = export_composite_bundle(
    auto_eq_bundles  = auto_eq_bundles,
    saturator_bundle = bundles / "saturator",
    ssl_comp_bundle  = bundles / "ssl_comp",
    out_dir          = out_dir,
    effect_name      = "Axon",
)
print(f"staged composite bundle at {out_dir}")
print(f"  effect_name: {meta.effect_name}")
print(f"  sample_rate: {meta.sample_rate}")
EOF

# Build the .clap.
mkdir -p "$(dirname "$OUT")"
bash "$REPO/native/clap/build.sh" axon "$STAGING" "$OUT"

# Optional install to system CLAP dir.
if [ "$INSTALL" = "1" ]; then
    INSTALL_DIR="$HOME/Library/Audio/Plug-Ins/CLAP"
    mkdir -p "$INSTALL_DIR"
    rm -rf "$INSTALL_DIR/$(basename "$OUT")"
    cp -R "$OUT" "$INSTALL_DIR/"
    echo "installed: $INSTALL_DIR/$(basename "$OUT")"
fi
