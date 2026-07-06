#!/usr/bin/env python3
"""Stage a Windows .clap from an export staging directory — the Windows
counterpart of package_linux.sh (python because Windows has no dependable
bash; stdlib only).

Two modes, same CLI as build.sh / package_linux.sh:
    single-model:   python package_windows.py <staging_dir> <out.clap>
    composite Axon: python package_windows.py axon <staging_dir> <out.clap>

WINDOWS RESOURCE CONVENTION (see src/resource_path.hpp for the loader side
and docs/future/active/windows-linux-builds.md "Implementation notes") — the
same two layouts as Linux; this script produces the DIRECTORY BUNDLE:

    <OUT>/                        (a plain directory named e.g. Axon.clap)
      <executable>.clap           (the plugin DLL, renamed — a CLAP on
                                   Windows is just a renamed DLL)
      onnxruntime.dll             (found next to the plugin because hosts
                                   load plugins with
                                   LOAD_WITH_ALTERED_SEARCH_PATH; see
                                   src/dyn_module.hpp)
      Resources/
        axon_meta.json + sub-bundle dirs + ui/    (composite), or
        plugin_meta.json + model.onnx             (single-model)

The FLAT layout (bare .clap with the resources side-by-side) is also
supported by the loader — copy the bundle's contents next to the binary.

Unlike package_linux.sh this script does NOT configure/build (clang-cl needs
the MSVC dev environment, which belongs to the caller — CI builds first);
it stages from an existing build/ and refuses to run without one. It DOES
run the same fast contract guards package_linux.sh runs before packaging.
"""

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD_DIR = HERE / "build"
EXE = ".exe" if sys.platform == "win32" else ""


def die(msg: str) -> None:
    print(f"package_windows.py: error: {msg}", file=sys.stderr)
    sys.exit(1)


def usage() -> None:
    print(
        "usage:\n"
        "  python package_windows.py <staging_dir> <out.clap>          # single-model\n"
        "  python package_windows.py axon <staging_dir> <out.clap>     # composite Axon",
        file=sys.stderr,
    )
    sys.exit(2)


def read_build_config(path: Path) -> dict:
    """Parse the KEY="value" lines of the CMake-generated build_config.sh."""
    cfg = {}
    for line in path.read_text().splitlines():
        m = re.match(r'^([A-Z_][A-Z0-9_]*)="(.*)"$', line.strip())
        if m:
            cfg[m.group(1)] = m.group(2)
    return cfg


def sub_bundle_dirs(meta: dict) -> list:
    """Mirror package_linux.sh: sub_bundles map + auto_eq classes."""
    dirs = set()
    for d in (meta.get("sub_bundles") or {}).values():
        dirs.add(d)
    for d in (meta.get("auto_eq", {}).get("classes") or {}).values():
        dirs.add(d)
    return sorted(dirs)


def main() -> None:
    args = sys.argv[1:]
    mode = "single"
    if args and args[0] == "axon":
        mode = "axon"
        args = args[1:]
    if len(args) != 2:
        usage()

    staging = Path(args[0]).resolve()
    out = Path(args[1])

    if mode == "single":
        if not (staging / "model.onnx").is_file() or not (staging / "plugin_meta.json").is_file():
            die(f"{staging} is missing model.onnx or plugin_meta.json")
        meta_path = staging / "plugin_meta.json"
    else:
        meta_path = staging / "axon_meta.json"
        if not meta_path.is_file():
            die(f"{staging} is missing axon_meta.json")

    meta = json.loads(meta_path.read_text())

    bundles = []
    if mode == "axon":
        bundles = sub_bundle_dirs(meta)
        if not bundles:
            die("axon_meta.json declares no sub_bundles or auto_eq.classes")
        for d in bundles:
            if not (staging / d).is_dir():
                die(f"{staging} is missing sub-bundle {d}/ (declared in axon_meta.json)")

    # ------------------------------------------------------------------ build
    build_config = BUILD_DIR / "build_config.sh"
    if not build_config.is_file():
        die(
            f"{build_config} not found — build first:\n"
            "  cmake -S native/clap -B native/clap/build -G Ninja "
            "-DCMAKE_BUILD_TYPE=Release "
            "-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl\n"
            "  cmake --build native/clap/build -j"
        )
    cfg = read_build_config(build_config)

    dylib = Path(cfg["AXON_CLAP_DYLIB" if mode == "axon" else "NABLAFX_CLAP_DYLIB"])
    ort_dll = Path(cfg["NABLAFX_CLAP_ORT_DYLIB"])
    if not dylib.is_file():
        die(f"{dylib} not built")
    if not ort_dll.is_file():
        die(f"{ort_dll} not found (FetchOnnxRuntime)")

    # Fast contract guards before anything is packaged (mirrors
    # package_linux.sh / build.sh).
    for guard in ("test_control_contract", "test_composite_meta_validate"):
        exe = BUILD_DIR / f"{guard}{EXE}"
        if not exe.is_file():
            die(f"{exe} not built (guard test)")
        r = subprocess.run([str(exe)])
        if r.returncode != 0:
            die(f"contract guard {guard} failed (rc={r.returncode})")

    # ------------------------------------------------------------------ stage
    model_id = meta["model_id"]
    effect_name = meta["effect_name"]
    executable = re.sub(r"[^a-zA-Z0-9_-]", "_", effect_name).lower().strip("_")

    if out.exists():
        shutil.rmtree(out)
    (out / "Resources").mkdir(parents=True)

    plugin_bin = out / f"{executable}.clap"
    shutil.copy2(dylib, plugin_bin)

    # Post-copy sanity check (mirrors build.sh / package_linux.sh): the
    # meta-filename string baked into the binary must match the mode.
    expected_meta = "axon_meta.json" if mode == "axon" else "plugin_meta.json"
    if expected_meta.encode() not in plugin_bin.read_bytes():
        die(
            f"{plugin_bin} is missing expected meta-string '{expected_meta}'.\n"
            f"       This means the wrong DLL was copied for MODE={mode}.\n"
            f"       (Likely a stale build/; try removing {BUILD_DIR} and rebuilding)"
        )

    # ONNX Runtime next to the plugin binary. The import lib records the DLL
    # name "onnxruntime.dll"; LOAD_WITH_ALTERED_SEARCH_PATH resolves it here.
    shutil.copy2(ort_dll, out / ort_dll.name)
    # onnxruntime.dll can lazily load the shared-providers helper; ship it if
    # the prebuilt has one (CPU-only inference works without it, but keep the
    # bundle self-contained).
    providers = ort_dll.parent / "onnxruntime_providers_shared.dll"
    if providers.is_file():
        shutil.copy2(providers, out / providers.name)

    if mode == "single":
        shutil.copy2(staging / "model.onnx", out / "Resources")
        shutil.copy2(staging / "plugin_meta.json", out / "Resources")
    else:
        shutil.copy2(staging / "axon_meta.json", out / "Resources")
        for d in bundles:
            shutil.copytree(staging / d, out / "Resources" / d)
        # Copy the WebUI so a future Windows GUI backend (Phase 3, WebView2)
        # can load it.
        shutil.copytree(HERE / "ui", out / "Resources" / "ui")

    print(f"built {out}")
    print(f"  mode:        {mode}")
    print(f"  effect_name: {effect_name}")
    print(f"  model_id:    {model_id}")
    print(f"  executable:  {executable}.clap")


if __name__ == "__main__":
    main()
