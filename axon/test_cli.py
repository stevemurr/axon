"""Unit tests for the axon CLI router + report generator.

Covers the parts with real logic: subcommand routing and pass-through
forwarding, the wav data-chunk comparator (the null-test protocol's core),
the axon-run/1 envelope written by Run, and report collection/generation.
Deliberately does NOT shell out to builds/benches — those are the underlying
tools' own responsibility.

Run directly (no pytest needed)::

    python3 axon/test_cli.py
"""
from __future__ import annotations

import contextlib
import io
import json
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from axon import cli, report  # noqa: E402

checks = 0


def check(name: str) -> None:
    global checks
    checks += 1
    print(f"  ok: {name}")


# ---------------------------------------------------------------- parser / routing

def test_parser_routing() -> None:
    p = cli.build_parser()

    args, extra = p.parse_known_args(["test", "--no-build", "-R", "meter"])
    assert args.fn is cli.cmd_test and args.no_build and extra == ["-R", "meter"]
    check("test routes + forwards ctest args without '--'")

    args, extra = p.parse_known_args(
        ["autoeq", "prepare", "--musdb", "--src", "/x", "--out", "/y"])
    assert args.fn is cli.cmd_autoeq_prepare
    assert extra == ["--musdb", "--src", "/x", "--out", "/y"]
    assert getattr(args, "passthrough", None)
    check("autoeq prepare routes + forwards flags")

    args, extra = p.parse_known_args(["autoeq", "export", "--run-dir", "/r"])
    assert args.fn is cli.cmd_autoeq_export and extra == ["--run-dir", "/r"]
    check("autoeq export routes")

    args, extra = p.parse_known_args(["autoeq", "probe", "--run-dir", "/r"])
    assert args.fn is cli.cmd_autoeq_probe and extra == ["--run-dir", "/r"]
    check("autoeq probe routes")

    args, extra = p.parse_known_args(
        ["train", "nablafx", "data=ssl_comp_musdb_trainval", "model=tcn/model_bb_tcn_ssl_comp"])
    assert args.fn is cli.cmd_train_nablafx
    assert extra == ["data=ssl_comp_musdb_trainval", "model=tcn/model_bb_tcn_ssl_comp"]
    check("train nablafx routes + forwards hydra overrides")

    args, extra = p.parse_known_args(["bench", "--json", "--buffers", "128"])
    assert args.fn is cli.cmd_bench and args.as_json and extra == ["--buffers", "128"]
    check("bench keeps own --json, forwards the rest")

    args, extra = p.parse_known_args(["eval", "null", "--set", "defaults"])
    assert args.fn is cli.cmd_eval_null and args.set == "defaults" and not extra
    check("eval null routes with own flags")

    for name, fn in [("build", cli.cmd_build), ("install", cli.cmd_install),
                     ("report", cli.cmd_report)]:
        args, _ = p.parse_known_args([name])
        assert args.fn is fn
        assert not getattr(args, "passthrough", None)
    check("build/install/report are NOT pass-through")

    # Non-pass-through subcommands must reject unknown args loudly (main()).
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            cli.main(["build", "--bogus"])
        raise AssertionError("expected SystemExit for unknown arg on build")
    except SystemExit as e:
        assert e.code == 2
    check("unknown args rejected on non-pass-through subcommands")


def test_rest_args_strips_separator() -> None:
    class A:
        rest = ["--", "-R", "meter"]
    assert cli.rest_args(A()) == ["-R", "meter"]

    class B:
        rest = ["-R", "meter"]
    assert cli.rest_args(B()) == ["-R", "meter"]
    check("rest_args tolerates an optional leading '--'")


def test_eval_sets_shape() -> None:
    assert set(cli.EVAL_SETS) == {"defaults", "full_chain_all", "adaptive_eq"}
    assert cli.EVAL_SETS["defaults"] == ""
    for params in cli.EVAL_SETS.values():
        assert " " not in params
    check("EVAL_SETS names/format stable")


# ---------------------------------------------------------------- wav comparator

def _wav(data_floats, extra_chunk=True) -> bytes:
    data = struct.pack(f"<{len(data_floats)}f", *data_floats)
    chunks = b""
    if extra_chunk:  # metadata chunk BEFORE data, like libsndfile's PEAK
        peak = struct.pack("<If", 12345, 0.5) + b"\x00\x00\x00\x00"
        chunks += b"PEAK" + struct.pack("<I", len(peak)) + peak
    chunks += b"data" + struct.pack("<I", len(data)) + data
    fmt = b"fmt " + struct.pack("<IHHIIHH", 16, 3, 2, 44100, 44100 * 8, 8, 32)
    body = b"WAVE" + fmt + chunks
    return b"RIFF" + struct.pack("<I", len(body)) + body


def test_wav_data_chunk_extraction() -> None:
    floats = [0.0, 0.25, -0.5, 1.0]
    with tempfile.TemporaryDirectory() as td:
        p1 = Path(td) / "a.wav"
        p2 = Path(td) / "b.wav"
        p1.write_bytes(_wav(floats, extra_chunk=True))
        p2.write_bytes(_wav(floats, extra_chunk=False))
        # Same audio, different metadata chunks -> data payloads must match.
        assert cli._wav_data(p1) == cli._wav_data(p2)
    check("_wav_data compares data chunk only (metadata ignored)")


def test_max_diff_dbfs() -> None:
    a = struct.pack("<3f", 0.0, 0.5, -0.25)
    b = struct.pack("<3f", 0.0, 0.5, -0.25 + 0.1)
    db = cli._max_diff_dbfs(a, b)
    assert abs(db - (-20.0)) < 0.1, db          # 0.1 amplitude = -20 dBFS
    assert cli._max_diff_dbfs(a, a) == float("-inf")
    check("_max_diff_dbfs math")


# ---------------------------------------------------------------- Run envelope

def test_run_envelope_and_report() -> None:
    real_repo = cli.REPO
    try:
        with tempfile.TemporaryDirectory() as td:
            cli.REPO = Path(td)
            r = cli.Run("test")
            rc = r.run([sys.executable, "-c", "print('hello from tool')"])
            assert rc == 0
            r.metrics = {"tests": 3, "failures": 1}
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = r.finish(False, "2/3 tests passed", exit_code=8, as_json=True)
            assert code == 8

            env = json.loads((r.dir / "result.json").read_text())
            assert env["schema"] == "axon-run/1"
            assert env["status"] == "fail"
            assert env["summary"] == "2/3 tests passed"
            assert env["metrics"] == {"tests": 3, "failures": 1}
            assert "output.log" in env["artifacts"]
            assert "hello from tool" in (r.dir / "output.log").read_text()
            check("Run writes a complete axon-run/1 envelope + output.log")

            latest = r.dir.parent / "latest"
            assert latest.is_symlink() and latest.resolve() == r.dir.resolve()
            check("latest symlink points at the newest run")

            footer = out.getvalue().splitlines()
            assert any(line.startswith("[axon] test: FAIL — 2/3 tests passed")
                       for line in footer)
            assert json.loads(footer[-1])["tool"] == "test"   # --json envelope
            check("uniform footer + --json envelope on stdout")

            # Run.finish auto-regenerates the HTML report.
            page = Path(td) / "artifacts" / "report" / "index.html"
            assert page.is_file() and "__DATA__" not in page.read_text()
            check("report auto-regenerated by Run.finish")
    finally:
        cli.REPO = real_repo


# ---------------------------------------------------------------- report module

def test_report_collect_and_generate() -> None:
    with tempfile.TemporaryDirectory() as td:
        repo = Path(td)
        for tool, stamps in {"test": ["20260101-010101", "20260102-020202"],
                             "bench": ["20260101-030303"]}.items():
            for i, stamp in enumerate(stamps):
                d = repo / "artifacts" / tool / stamp
                d.mkdir(parents=True)
                (d / "result.json").write_text(json.dumps({
                    "schema": "axon-run/1", "tool": tool,
                    "status": "pass" if i else "fail",
                    "summary": f"{tool} run {i}", "duration_s": 1.5,
                    "git": {"rev": "abc1234", "dirty": False},
                    "metrics": {}, "details": {}, "artifacts": [],
                }))
        # Noise that must be ignored: latest symlink, report dir, junk run dir.
        (repo / "artifacts" / "test" / "latest").symlink_to("20260102-020202")
        (repo / "artifacts" / "report").mkdir()
        (repo / "artifacts" / "test" / "junk").mkdir()

        tools = report.collect(repo)
        assert set(tools) == {"test", "bench"}
        assert [r["stamp"] for r in tools["test"]] == \
            ["20260102-020202", "20260101-010101"]           # newest first
        assert tools["test"][0]["status"] == "pass"
        assert tools["test"][0]["path"] == "../test/20260102-020202/"
        check("collect groups, sorts newest-first, skips latest/report/junk")

        out = report.generate(repo)
        html = out.read_text()
        assert out == repo / "artifacts" / "report" / "index.html"
        assert "__DATA__" not in html
        embedded = html.split('<script id="data" type="application/json">')[1]
        embedded = embedded.split("</script>")[0].replace("<\\/", "</")
        data = json.loads(embedded)
        assert data["meta"]["tool_order"] == ["test", "bench"]
        assert len(data["tools"]["test"]) == 2
        check("generate embeds parseable data with stable tool order")


if __name__ == "__main__":
    test_parser_routing()
    test_rest_args_strips_separator()
    test_eval_sets_shape()
    test_wav_data_chunk_extraction()
    test_max_diff_dbfs()
    test_run_envelope_and_report()
    test_report_collect_and_generate()
    print(f"test_cli: {checks} checks passed")
