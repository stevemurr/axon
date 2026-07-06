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
    uv run axon autoeq prepare --musdb --src ... --out ...   (training host)
    uv run axon autoeq train [fanout args...]                 (training host)
    uv run axon autoeq export --run-dir ... --out ...         (training host)
    uv run axon autoeq probe --run-dir ...                     (training host)
    uv run axon train nablafx <hydra overrides...>             (training host)
    uv run axon release --major|--minor|--patch [--dry-run]
    uv run axon report [--open]

Output convention (test / bench / coverage / eval): every run writes
artifacts/<tool>/<timestamp>/ containing result.json (the uniform axon-run/1
envelope: status, summary, metrics, git state) + output.log (full tool
output) + tool-specific artifacts; artifacts/<tool>/latest always points at
the newest run, and every command ends with the same one-line footer:

    [axon] <tool>: PASS|FAIL — <summary> -> artifacts/<tool>/<ts>/

Pass --json (before any pass-through args) to also print the envelope to
stdout for CI. artifacts/ is gitignored; the only TRACKED result artifact is
bench/baseline.json, which is an input (the regression reference), not an
output.

Do not add flow logic here that belongs in the underlying scripts — this file
should stay a router plus the null-compare protocol.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CLAP = REPO / "native" / "clap"
BUILD_DIR = CLAP / "build"
BUNDLE = REPO / "build" / "Axon.clap"
INSTALLED = Path.home() / "Library" / "Audio" / "Plug-Ins" / "CLAP" / "Axon.clap"
FIXTURE = CLAP / "bench" / "fixtures" / "bench_input_20s.wav"

# Reference param sets for A/B null evaluation. "adaptive_eq" involves the ORT
# paths, which are subject to a known same-binary run-to-run nondeterminism at
# -86..-99 dBFS — see docs/future/*/ort_render_nondeterminism.md.
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


def run_logged(cmd, log_path: Path, env=None, quiet: bool = False) -> int:
    """Run a command streaming stdout+stderr to the console AND a log file."""
    line = " ".join(str(c) for c in cmd)
    if not quiet:
        print(f"[axon] $ {line}", flush=True)
    with open(log_path, "a") as log:
        log.write(f"$ {line}\n")
        proc = subprocess.Popen([str(c) for c in cmd], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for out in proc.stdout:
            if not quiet:
                sys.stdout.write(out)
            log.write(out)
        return proc.wait()


def _git_state() -> dict:
    try:
        rev = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True).stdout.strip()
        dirty = bool(subprocess.run(["git", "-C", REPO, "status", "--porcelain"],
                                    capture_output=True, text=True).stdout.strip())
        return {"rev": rev, "dirty": dirty}
    except Exception:
        return {}


class Run:
    """The axon-run/1 output convention (see module docstring)."""

    def __init__(self, tool: str):
        self.tool = tool
        self.t0 = time.time()
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.dir = REPO / "artifacts" / tool / stamp
        self.dir.mkdir(parents=True, exist_ok=True)
        self.log = self.dir / "output.log"
        self.metrics: dict = {}
        self.details: dict = {}

    def run(self, cmd, env=None, quiet: bool = False) -> int:
        return run_logged(cmd, self.log, env=env, quiet=quiet)

    def keep(self, src: Path, name: str | None = None) -> None:
        shutil.copy2(src, self.dir / (name or Path(src).name))

    def finish(self, ok: bool, summary: str, exit_code: int | None = None,
               as_json: bool = False) -> int:
        envelope = {
            "schema": "axon-run/1",
            "tool": self.tool,
            "status": "pass" if ok else "fail",
            "summary": summary,
            "duration_s": round(time.time() - self.t0, 2),
            "git": _git_state(),
            "metrics": self.metrics,
            "details": self.details,
            "artifacts": sorted(p.name for p in self.dir.iterdir()
                                if p.name != "result.json"),
        }
        (self.dir / "result.json").write_text(json.dumps(envelope, indent=2) + "\n")
        latest = self.dir.parent / "latest"
        latest.unlink(missing_ok=True)
        latest.symlink_to(self.dir.name)
        try:
            # Keep the HTML view current on every run; never fail a run over it.
            from axon import report
            report.generate(REPO)
        except Exception:
            pass
        print(f"[axon] {self.tool}: {'PASS' if ok else 'FAIL'} — {summary} "
              f"-> {self.dir.relative_to(REPO)}/")
        if as_json:
            print(json.dumps(envelope))
        return exit_code if exit_code is not None else (0 if ok else 1)


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
    r = Run("test")
    junit = r.dir / "junit.xml"
    rc = r.run(["ctest", "--test-dir", BUILD_DIR, "--output-on-failure",
                "--output-junit", junit, *rest_args(args)])
    total = failed = None
    try:
        import xml.etree.ElementTree as ET
        root = ET.parse(junit).getroot()
        total = int(root.get("tests", 0))
        failed = int(root.get("failures", 0))
        r.metrics = {"tests": total, "failures": failed,
                     "skipped": int(root.get("skipped") or root.get("disabled") or 0)}
    except Exception:
        pass
    ok = rc == 0
    summary = (f"{total - failed}/{total} tests passed" if total is not None
               else ("suite passed" if ok else "suite FAILED"))
    return r.finish(ok, summary, exit_code=rc, as_json=args.as_json)


def cmd_bench(args) -> int:
    r = Run("bench")
    rest = rest_args(args)
    if not any(a.startswith("--out") for a in rest):
        rest += ["--out", str(r.dir / "bench.json")]
    if not any(a.startswith("--summary") for a in rest):
        rest += ["--summary", str(r.dir / "bench.md")]
    rc = r.run([sys.executable, CLAP / "bench" / "run_bench.py", *rest])
    try:
        cells = json.loads((r.dir / "bench.json").read_text()).get("results", [])
        r.metrics = {"cells": len(cells),
                     "deadline_misses": sum(c.get("deadline_misses", 0) for c in cells)}
    except Exception:
        pass
    summary = {0: f"{r.metrics.get('cells', '?')} cells, no regressions",
               2: "REGRESSION vs baseline"}.get(rc, "bench error")
    return r.finish(rc == 0, summary, exit_code=rc, as_json=args.as_json)


def cmd_coverage(args) -> int:
    r = Run("coverage")
    rc = r.run(["bash", CLAP / "bench" / "run_coverage.sh", *rest_args(args)])
    report = CLAP / "build-cov" / "coverage-report.txt"
    line_pct = None
    if report.exists():
        r.keep(report)
        for line in report.read_text().splitlines():
            if line.startswith("TOTAL"):
                # llvm-cov report TOTAL row: pct columns are regions,
                # functions, lines (in that order).
                pcts = [t.rstrip("%") for t in line.split() if t.endswith("%")]
                if pcts:
                    line_pct = float(pcts[2] if len(pcts) >= 3 else pcts[-1])
        if line_pct is not None:
            r.metrics["line_pct"] = line_pct
    r.details["html"] = str(CLAP / "build-cov" / "coverage-html" / "index.html")
    ok = rc == 0
    summary = (f"{line_pct}% line coverage" if line_pct is not None
               else ("done" if ok else "coverage FAILED"))
    return r.finish(ok, summary, exit_code=rc, as_json=args.as_json)


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


def _render(r: "Run", bundle: Path, params: str, out: Path) -> int:
    cmd = [BUILD_DIR / "axon_bench", "--plugin", bundle, "--in", FIXTURE,
           "--buffer", "128", "--iters", "1", "--warmup", "0", "--out", out, "--json"]
    if params:
        cmd += ["--params", params]
    return r.run(cmd, quiet=True)


def cmd_eval_null(args) -> int:
    """Render the current tree's bundle vs a reference bundle and compare."""
    ref = Path(args.against).expanduser()
    if not (ref / "Contents" / "MacOS").is_dir():
        return die(f"reference bundle not found: {ref}")
    if not args.no_build and (rc := cmd_build(argparse.Namespace())):
        return rc
    r = Run("eval-null")
    r.details["against"] = str(ref)
    sets = EVAL_SETS if args.set == "all" else {args.set: EVAL_SETS[args.set]}
    failed, flakes = [], 0
    with tempfile.TemporaryDirectory(prefix="axon-eval-") as td:
        tdir = Path(td)
        for name, params in sets.items():
            # Retry protocol (docs/future ort_render_nondeterminism doc):
            # only a REPRODUCIBLE mismatch across attempts indicts the change.
            verdicts = []
            for attempt in range(3):
                a = tdir / f"{name}_ref_{attempt}.wav"
                b = tdir / f"{name}_cur_{attempt}.wav"
                if _render(r, ref, params, a) or _render(r, BUNDLE, params, b):
                    return r.finish(False, f"render failed for set '{name}'",
                                    as_json=args.as_json)
                da, db = _wav_data(a), _wav_data(b)
                if da == db:
                    verdicts.append("IDENTICAL")
                    break
                verdicts.append(f"{_max_diff_dbfs(da, db):.1f} dBFS")
                # Keep the differing pair for inspection.
                r.keep(a); r.keep(b)
            ok = verdicts[-1] == "IDENTICAL"
            flake = ok and len(verdicts) > 1
            flakes += flake
            r.details[name] = verdicts
            print(f"[axon] {name}: {verdicts[-1]}"
                  + (f"  (flaked then matched: {verdicts[:-1]} — known ORT nondeterminism)" if flake else ""))
            if not ok:
                failed.append(name)
                print(f"[axon] {name}: REPRODUCIBLE mismatch across {len(verdicts)} attempts: {verdicts}")
    r.metrics = {"sets": len(sets), "failed": len(failed), "flakes": flakes}
    summary = (f"non-null sets: {', '.join(failed)}" if failed
               else f"{len(sets)}/{len(sets)} sets null"
                    + (f" ({flakes} ORT flake(s) resolved on retry)" if flakes else ""))
    return r.finish(not failed, summary, as_json=args.as_json)


def cmd_eval_ssl_comp(args) -> int:
    r = Run("eval-ssl-comp")
    rc = r.run([sys.executable, REPO / "scripts" / "verify_ssl_comp_model.py",
                *rest_args(args)])
    # The verify script's machine-readable line: "RESULT k=v k=v ..."
    if r.log.exists():
        for line in reversed(r.log.read_text().splitlines()):
            if line.startswith("RESULT "):
                for kv in line[len("RESULT "):].split():
                    k, _, v = kv.partition("=")
                    try:
                        r.metrics[k] = float(v)
                    except ValueError:
                        r.metrics[k] = v
                break
    ok = rc == 0
    return r.finish(ok, "all sizing invariants PASS" if ok else "invariant checks FAILED",
                    exit_code=rc, as_json=args.as_json)


# ------------------------------------------------- autoeq model lifecycle

def _require_train_extra(what: str, wraps: str) -> int | None:
    try:
        import nablafx  # noqa: F401
        return None
    except ImportError:
        return die(
            f"nablafx is not installed — '{what}' runs on the GPU host.\n"
            f"  There:  uv sync --extra train && uv run axon {what} ...\n"
            f"  This wraps {wraps}."
        )


def cmd_autoeq_prepare(args) -> int:
    if (rc := _require_train_extra("autoeq prepare",
                                   "scripts/prepare_auto_eq_data.py")) is not None:
        return rc
    return run([sys.executable, REPO / "scripts" / "prepare_auto_eq_data.py",
                *rest_args(args)])


def cmd_autoeq_train(args) -> int:
    if (rc := _require_train_extra(
            "autoeq train", "scripts/train_auto_eq_musdb_fanout.sh (expects the "
            "/shared/artifacts data layout)")) is not None:
        return rc
    return run(["bash", REPO / "scripts" / "train_auto_eq_musdb_fanout.sh",
                *rest_args(args)])


def cmd_autoeq_probe(args) -> int:
    # Bundle mode (default) runs anywhere with onnxruntime; the script itself
    # gates --run-dir mode on torch/nablafx (training host).
    return run([sys.executable, REPO / "scripts" / "probe_auto_eq_adaptivity.py",
                *rest_args(args)])


def cmd_train_nablafx(args) -> int:
    """Raw nablafx training run with hydra overrides passed through verbatim,
    e.g.: axon train nablafx data=ssl_comp_musdb_trainval model=tcn/model_bb_tcn_ssl_comp"""
    if (rc := _require_train_extra(
            "train nablafx", "the nablafx trainer (hydra overrides pass "
            "through verbatim; recipes per piece are in the README Training "
            "section — note the auto-EQ 5-class fan-out has its own command, "
            "axon autoeq train)")) is not None:
        return rc
    exe = shutil.which("nablafx")
    if not exe:
        return die("nablafx trainer not on PATH (comes with the train extra)")
    return run([exe, *rest_args(args)])


def cmd_autoeq_export(args) -> int:
    if (rc := _require_train_extra(
            "autoeq export", "nablafx-export (per-class bundle: model.onnx + "
            "plugin_meta.json into weights/axon_bundle/auto_eq_<class>)")) is not None:
        return rc
    exe = shutil.which("nablafx-export")
    if not exe:
        return die("nablafx-export not on PATH (comes with the nablafx install)")
    return run([exe, *rest_args(args)])


def bump_version(latest: str, part: str) -> str:
    """Next semver from the latest 'vX.Y.Z' tag ('' means no tags yet)."""
    if not latest:
        latest = "v0.0.0"
    core = latest.lstrip("v").split("-")[0]
    try:
        major, minor, patch = (int(x) for x in core.split("."))
    except ValueError:
        raise SystemExit(f"[axon] error: cannot parse latest tag '{latest}' as semver")
    if part == "major":
        return f"v{major + 1}.0.0"
    if part == "minor":
        return f"v{major}.{minor + 1}.0"
    return f"v{major}.{minor}.{patch + 1}"


def cmd_release(args) -> int:
    """Compute the next version, gate on the suite, tag + push (which triggers
    the multi-platform release workflow)."""
    part = "major" if args.major else ("minor" if args.minor else "patch")
    def git(*a):
        return subprocess.run(["git", "-C", REPO, *a],
                              capture_output=True, text=True).stdout.strip()
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    if branch != "main":
        return die(f"releases are cut from main (currently on '{branch}')")
    if git("status", "--porcelain"):
        return die("working tree is dirty — commit or stash first")
    subprocess.run(["git", "-C", REPO, "fetch", "origin", "main", "--quiet"])
    if git("rev-parse", "HEAD") != git("rev-parse", "origin/main"):
        return die("main is not in sync with origin/main — pull/push first")
    tags = [t for t in git("tag", "-l", "v*").splitlines() if t]
    latest = max(tags, key=lambda t: [int(x) for x in
                 t.lstrip("v").split("-")[0].split(".")]) if tags else ""
    nxt = bump_version(latest, part)
    print(f"[axon] release: {latest or '(no tags yet)'} -> {nxt} ({part} bump)")
    if args.dry_run:
        print(f"[axon] dry run — would: run the test suite, then "
              f"scripts/cut_release.sh --yes {nxt.lstrip('v')} "
              f"(tag + push -> multi-platform release workflow)")
        return 0
    if not args.skip_test:
        if (rc := cmd_test(argparse.Namespace(no_build=False, as_json=False, rest=[]))):
            return die("test suite failed — release blocked", rc)
    return run(["bash", REPO / "scripts" / "cut_release.sh", "--yes", nxt.lstrip("v")])


def cmd_report(args) -> int:
    from axon import report
    out = report.generate(REPO)
    print(f"[axon] report: {out.relative_to(REPO)}")
    if args.open:
        subprocess.call(["open", str(out)])
    return 0


# ---------------------------------------------------------------- main

def build_parser() -> argparse.ArgumentParser:
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
    t.add_argument("--json", dest="as_json", action="store_true",
                   help="also print the result envelope to stdout")
    t.set_defaults(fn=cmd_test, passthrough="ctest")

    be = sub.add_parser("bench", help="scenario/buffer benchmark matrix (bench/run_bench.py)")
    be.add_argument("--json", dest="as_json", action="store_true",
                    help="also print the result envelope (place before pass-through args)")
    be.set_defaults(fn=cmd_bench, passthrough="run_bench.py")

    c = sub.add_parser("coverage", help="llvm-cov over the test suite (bench/run_coverage.sh)")
    c.add_argument("--json", dest="as_json", action="store_true")
    c.set_defaults(fn=cmd_coverage, passthrough="run_coverage.sh")

    e = sub.add_parser("eval", help="evaluations")
    esub = e.add_subparsers(dest="eval_cmd", required=True)
    en = esub.add_parser("null", help="A/B null: current tree vs a reference bundle")
    en.add_argument("--against", default=str(INSTALLED),
                    help=f"reference .clap bundle (default: {INSTALLED})")
    en.add_argument("--set", default="all", choices=[*EVAL_SETS, "all"])
    en.add_argument("--no-build", action="store_true")
    en.add_argument("--json", dest="as_json", action="store_true")
    en.set_defaults(fn=cmd_eval_null)
    es = esub.add_parser("ssl-comp", help="ssl_comp model sizing invariants (scripts/verify_ssl_comp_model.py)")
    es.add_argument("--json", dest="as_json", action="store_true")
    es.set_defaults(fn=cmd_eval_ssl_comp, passthrough="verify_ssl_comp_model.py")

    aq = sub.add_parser("autoeq", help="auto-EQ model lifecycle (GPU host; needs --extra train)")
    aqsub = aq.add_subparsers(dest="autoeq_cmd", required=True)
    aqp = aqsub.add_parser("prepare", add_help=False,
                           help="dataset prep (args pass through to scripts/prepare_auto_eq_data.py)")
    aqp.set_defaults(fn=cmd_autoeq_prepare, passthrough="prepare_auto_eq_data.py")
    aqt = aqsub.add_parser("train", help="train all five class models (scripts/train_auto_eq_musdb_fanout.sh)")
    aqt.set_defaults(fn=cmd_autoeq_train, passthrough="train fan-out")
    aqe = aqsub.add_parser("export", add_help=False,
                           help="export a trained run to a per-class bundle (args pass through to nablafx-export)")
    aqe.set_defaults(fn=cmd_autoeq_export, passthrough="nablafx-export")
    aqpr = aqsub.add_parser("probe", add_help=False,
                            help="adaptivity probe: shipped bundles by default (runs anywhere), "
                                 "or --run-dir <hydra_run> on the training host")
    aqpr.set_defaults(fn=cmd_autoeq_probe, passthrough="probe_auto_eq_adaptivity.py")

    tn = sub.add_parser("train", help="training runs (GPU host; needs --extra train)")
    tnsub = tn.add_subparsers(dest="train_cmd", required=True)
    tnn = tnsub.add_parser("nablafx", add_help=False,
                           help="raw nablafx run; hydra overrides pass through verbatim "
                                "(e.g. data=ssl_comp_musdb_trainval model=tcn/model_bb_tcn_ssl_comp)")
    tnn.set_defaults(fn=cmd_train_nablafx, passthrough="nablafx")

    rl = sub.add_parser("release", help="bump version, gate on the suite, tag + push (triggers the multi-platform release)")
    grp = rl.add_mutually_exclusive_group(required=True)
    grp.add_argument("--major", action="store_true")
    grp.add_argument("--minor", action="store_true")
    grp.add_argument("--patch", action="store_true")
    rl.add_argument("--dry-run", action="store_true", help="print the computed version and stop")
    rl.add_argument("--skip-test", action="store_true", help="skip the local suite gate (CI still gates)")
    rl.set_defaults(fn=cmd_release)

    rp = sub.add_parser("report", help="regenerate the HTML run report from artifacts/")
    rp.add_argument("--open", action="store_true", help="open it in the browser")
    rp.set_defaults(fn=cmd_report)
    return p


def main(argv=None) -> int:
    p = build_parser()
    # Pass-through subcommands forward every argument they don't recognize to
    # the underlying tool — no `--` separator needed (argparse REMAINDER can't
    # do this when the first forwarded token is a flag).
    args, extra = p.parse_known_args(argv)
    if extra and not getattr(args, "passthrough", None):
        p.error(f"unrecognized arguments: {' '.join(extra)}")
    args.rest = extra
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
