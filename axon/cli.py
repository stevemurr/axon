"""axon — single entrypoint for build / test / bench / coverage / eval / train.

Every subcommand is a THIN delegation to the canonical tool it names (printed
before running), so there is exactly one implementation of each flow:

    uv run axon build [--instrumented]
    uv run axon install [--mac]
    uv run axon test  [ctest args...]
    uv run axon bench [run_bench.py args...]
    uv run axon coverage
    uv run axon eval null [--against BUNDLE] [--set NAME]
    uv run axon eval ssl-comp
    uv run axon train [fanout args...]          (training host)

Do not add flow logic here that belongs in the underlying scripts — this file
should stay a router plus the null-compare protocol.
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CLAP = REPO / "native" / "clap"
BUILD_DIR = CLAP / "build"
BUNDLE = REPO / "build" / "Axon.clap"
INSTALLED = Path.home() / "Library" / "Audio" / "Plug-Ins" / "CLAP" / "Axon.clap"
FIXTURE = CLAP / "bench" / "fixtures" / "bench_input_20s.wav"

# Reference param sets for A/B null evaluation. "adaptive_eq" involves the ORT
# paths, which are subject to a known same-binary run-to-run nondeterminism at
# -86..-99 dBFS — see native/clap/docs/investigations/ort_render_nondeterminism.md.
# The compare below implements that doc's retry protocol.
EVAL_SETS = {
    "defaults": "",
    "full_chain_all": (
        "EQ=1.0,EQR=1.0,EQB=1.0,EQS=100,SDR=12,SVO=0,SMX=1.0,STH=-12,SBS=0,"
        "SSC=1.0,CLS=4,LVL=1.0,LVT=-14,OLV=1.0,OLT=-14,TRM=0,"
        "SEQ_ON=1,SEQ_LF_G=3,SEQ_HMF_G=-2.5,SEQ_HF_G=2,SEQ_HPF_ON=1,"
        "BMI=1.0,BMF=120,RVB_MIX=0.25,WID_ON=1,WID_AMT=1.25,WID_AIR=0.3"
    ),
    "adaptive_eq": "EQ=1.0,EQ_ENGINE=1,SSC=1.0,CLS=4,SDR=0,SVO=0,SMX=0,LVL=0,OLV=0,TRM=0",
}


def run(cmd, **kw) -> int:
    print(f"[axon] $ {' '.join(str(c) for c in cmd)}", flush=True)
    return subprocess.call([str(c) for c in cmd], **kw)


def die(msg: str, code: int = 1) -> int:
    print(f"[axon] error: {msg}", file=sys.stderr)
    return code


def rest_args(args) -> list:
    """Pass-through args minus the conventional `--` separator."""
    r = list(getattr(args, "rest", []) or [])
    if r and r[0] == "--":
        r.pop(0)
    return r


# ---------------------------------------------------------------- build / test

def cmd_build(args) -> int:
    cmd = ["bash", REPO / "scripts" / "install_axon_mac.sh", "--no-install"]
    env = os.environ.copy()
    if getattr(args, "instrumented", False):
        # Bench-only build; the installer refuses to install these by design.
        env["AXON_STAGE_TIMING"] = "1"
    return run(cmd, env=env)


def cmd_install(args) -> int:
    # --mac is today's only platform (the CMake gate is arm64-macOS-only);
    # the flag exists so future platforms slot in as siblings.
    if not args.mac:
        return die("only --mac exists today (CMakeLists gates on arm64 macOS)")
    return run(["bash", REPO / "scripts" / "install_axon_mac.sh"])


def cmd_test(args) -> int:
    if not args.no_build and (rc := cmd_build(argparse.Namespace())):
        return rc
    return run(["ctest", "--test-dir", BUILD_DIR, "--output-on-failure", *rest_args(args)])


def cmd_bench(args) -> int:
    return run([sys.executable, CLAP / "bench" / "run_bench.py", *rest_args(args)])


def cmd_coverage(args) -> int:
    return run(["bash", CLAP / "bench" / "run_coverage.sh", *rest_args(args)])


# ---------------------------------------------------------------- eval

def _wav_data(path: Path) -> bytes:
    """Extract the RIFF data chunk. Never compare whole files: the PEAK
    metadata chunk embeds a timestamp."""
    b = path.read_bytes()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE", f"not a wav: {path}"
    i = 12
    while i < len(b) - 8:
        cid = b[i:i + 4]
        (sz,) = struct.unpack("<I", b[i + 4:i + 8])
        if cid == b"data":
            return b[i + 8:i + 8 + sz]
        i += 8 + sz + (sz & 1)
    raise ValueError(f"no data chunk in {path}")


def _max_diff_dbfs(a: bytes, b: bytes) -> float:
    import math
    fa = struct.unpack(f"<{len(a) // 4}f", a)
    fb = struct.unpack(f"<{len(b) // 4}f", b)
    mx = max((abs(x - y) for x, y in zip(fa, fb)), default=0.0)
    return 20.0 * math.log10(mx) if mx > 0 else float("-inf")


def _render(bundle: Path, params: str, out: Path) -> int:
    cmd = [BUILD_DIR / "axon_bench", "--plugin", bundle, "--in", FIXTURE,
           "--buffer", "128", "--iters", "1", "--warmup", "0", "--out", out, "--json"]
    if params:
        cmd += ["--params", params]
    return run(cmd, stdout=subprocess.DEVNULL)


def cmd_eval_null(args) -> int:
    """Render the current tree's bundle vs a reference bundle and compare."""
    ref = Path(args.against).expanduser()
    if not (ref / "Contents" / "MacOS").is_dir():
        return die(f"reference bundle not found: {ref}")
    if not args.no_build and (rc := cmd_build(argparse.Namespace())):
        return rc
    sets = EVAL_SETS if args.set == "all" else {args.set: EVAL_SETS[args.set]}
    failed = []
    with tempfile.TemporaryDirectory(prefix="axon-eval-") as td:
        tdir = Path(td)
        for name, params in sets.items():
            # Retry protocol (investigations/ort_render_nondeterminism.md):
            # only a REPRODUCIBLE mismatch across attempts indicts the change.
            verdicts = []
            for attempt in range(3):
                a, b = tdir / f"{name}_ref_{attempt}.wav", tdir / f"{name}_cur_{attempt}.wav"
                if _render(ref, params, a) or _render(BUNDLE, params, b):
                    return die(f"render failed for set '{name}'")
                da, db = _wav_data(a), _wav_data(b)
                if da == db:
                    verdicts.append("IDENTICAL")
                    break
                verdicts.append(f"{_max_diff_dbfs(da, db):.1f} dBFS")
            ok = verdicts[-1] == "IDENTICAL"
            flake = ok and len(verdicts) > 1
            print(f"[axon] {name}: {verdicts[-1]}"
                  + (f"  (flaked then matched: {verdicts[:-1]} — known ORT nondeterminism)" if flake else ""))
            if not ok:
                failed.append(name)
                print(f"[axon] {name}: REPRODUCIBLE mismatch across {len(verdicts)} attempts: {verdicts}")
    if failed:
        return die(f"non-null sets: {', '.join(failed)}")
    print("[axon] eval null: PASS")
    return 0


def cmd_eval_ssl_comp(args) -> int:
    return run([sys.executable, REPO / "scripts" / "verify_ssl_comp_model.py", *rest_args(args)])


# ---------------------------------------------------------------- train

def cmd_train(args) -> int:
    try:
        import nablafx  # noqa: F401
    except ImportError:
        return die(
            "nablafx is not installed — training runs on the GPU host.\n"
            "  There:  uv sync --extra train && uv run axon train ...\n"
            "  This wraps scripts/train_auto_eq_musdb_fanout.sh (expects the\n"
            "  /shared/artifacts data layout)."
        )
    return run(["bash", REPO / "scripts" / "train_auto_eq_musdb_fanout.sh", *rest_args(args)])


# ---------------------------------------------------------------- main

def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="axon", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build", help="build the .clap bundle (scripts/install_axon_mac.sh --no-install)")
    b.add_argument("--instrumented", action="store_true",
                   help="bench-only AXON_STAGE_TIMING build (never installable)")
    b.set_defaults(fn=cmd_build)

    ins = sub.add_parser("install", help="build + install into the DAW plugin folder")
    ins.add_argument("--mac", action="store_true", default=True,
                     help="macOS (~/Library/Audio/Plug-Ins/CLAP) — the default and only platform today")
    ins.set_defaults(fn=cmd_install)

    t = sub.add_parser("test", help="build, then run the full suite via ctest")
    t.add_argument("--no-build", action="store_true", help="run against existing binaries")
    t.add_argument("rest", nargs=argparse.REMAINDER, help="extra ctest args (e.g. -R name)")
    t.set_defaults(fn=cmd_test)

    be = sub.add_parser("bench", help="scenario/buffer benchmark matrix (bench/run_bench.py)")
    be.add_argument("rest", nargs=argparse.REMAINDER)
    be.set_defaults(fn=cmd_bench)

    c = sub.add_parser("coverage", help="llvm-cov over the test suite (bench/run_coverage.sh)")
    c.add_argument("rest", nargs=argparse.REMAINDER)
    c.set_defaults(fn=cmd_coverage)

    e = sub.add_parser("eval", help="evaluations")
    esub = e.add_subparsers(dest="eval_cmd", required=True)
    en = esub.add_parser("null", help="A/B null: current tree vs a reference bundle")
    en.add_argument("--against", default=str(INSTALLED),
                    help=f"reference .clap bundle (default: {INSTALLED})")
    en.add_argument("--set", default="all", choices=[*EVAL_SETS, "all"])
    en.add_argument("--no-build", action="store_true")
    en.set_defaults(fn=cmd_eval_null)
    es = esub.add_parser("ssl-comp", help="ssl_comp model sizing invariants (scripts/verify_ssl_comp_model.py)")
    es.add_argument("rest", nargs=argparse.REMAINDER)
    es.set_defaults(fn=cmd_eval_ssl_comp)

    tr = sub.add_parser("train", help="auto-EQ training fan-out (GPU host; needs --extra train)")
    tr.add_argument("rest", nargs=argparse.REMAINDER)
    tr.set_defaults(fn=cmd_train)

    args = p.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
