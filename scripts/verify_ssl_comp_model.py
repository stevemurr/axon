#!/usr/bin/env python3
"""Empirically verify the ssl_comp ONNX TCN sizing invariants.

History: the model originally shipped with a static [1,1,2048] input while the
plugin consumed only the last 1024 (kSslHop) of 1418 outputs — 27.8% of output
compute discarded per hop. On 2026-07-05 the model was resized in place to
trace_len=1655 (graph surgery: residual-crop Slice `ends` rewritten to the
length-relative -1, input/output time dims set to 1655/1025 — byte-identical
over the consumed range vs the 2048 original, ~20% faster forward). This
script verifies the standing invariants for the CURRENT model: N below must
match plugin_meta.json trace_len.

Session options mirror the plugin (native/clap/src/axon_plugin.cpp ~395-399):
intra=1, inter=1, sequential, ORT_ENABLE_ALL.

Usage: python3 scripts/verify_ssl_comp_model.py [path/to/model.onnx]
Exits nonzero if any check FAILs.
"""
import sys
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort

REPO = Path(__file__).resolve().parents[1]
DEFAULT_MODEL = REPO / "weights" / "axon_bundle" / "ssl_comp" / "model.onnx"

N = 1655            # static trace_len — MUST match plugin_meta.json
RF = 631            # receptive_field (plugin_meta.json)
HOP = 1024          # kSslHop
OUT_LEN = N - (RF - 1)          # expected output samples (1025 at N=1655)
CONSUMED0 = OUT_LEN - HOP       # first consumed output index (1 at N=1655)
SHORT_T = HOP + RF - 1          # 1654: minimal window for the consumed range

failures = []


def check(name, ok, evidence):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {evidence}")
    if not ok:
        failures.append(name)


def make_session(model_source):
    so = ort.SessionOptions()
    so.intra_op_num_threads = 1
    so.inter_op_num_threads = 1
    so.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    return ort.InferenceSession(model_source, sess_options=so,
                                providers=["CPUExecutionProvider"])


def run(sess, x):
    return sess.run(["audio_out"], {"audio_in": x})[0]


def changed_indices(a, b):
    """Absolute output indices where float32 bytes differ."""
    return np.nonzero(a.reshape(-1).view(np.uint32) != b.reshape(-1).view(np.uint32))[0]


def fmt_set(idx):
    if idx.size == 0:
        return "{} (empty)"
    lo, hi = int(idx.min()), int(idx.max())
    dense = idx.size == (hi - lo + 1)
    return f"[{lo}..{hi}] count={idx.size}" + ("" if dense else " (NON-CONTIGUOUS)")


def main():
    model_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_MODEL
    print(f"model: {model_path}")
    sess = make_session(str(model_path))

    # ---------------- (a) STATIC SHAPE ----------------
    inp = sess.get_inputs()[0]
    out = sess.get_outputs()[0]
    print(f"declared input  '{inp.name}' shape={inp.shape} dtype={inp.type}")
    print(f"declared output '{out.name}' shape={out.shape} dtype={out.type}")
    check("a1 input name/shape",
          inp.name == "audio_in" and list(inp.shape) == [1, 1, N],
          f"got name='{inp.name}' shape={inp.shape}, expected 'audio_in' [1,1,{N}]")

    x_short = np.zeros((1, 1, SHORT_T), dtype=np.float32)
    try:
        y_short = run(sess, x_short)
        check("a2 reject [1,1,1654]", False,
              f"ACCEPTED shorter input; output shape={y_short.shape} "
              "(static-shape claim is FALSE)")
    except Exception as e:  # noqa: BLE001
        msg = " ".join(str(e).split())
        is_invalid = ("INVALID_ARGUMENT" in msg or "Invalid" in type(e).__name__
                      or "invalid" in msg.lower())
        check("a2 reject [1,1,1654]", is_invalid,
              f"rejected with {type(e).__name__}: {msg[:220]}")

    # ---------------- (b) CAUSALITY / LOCALITY ----------------
    rng = np.random.default_rng(1234)
    x = rng.standard_normal((1, 1, N)).astype(np.float32)
    y0 = run(sess, x)
    check("b0 output length", y0.shape == (1, 1, OUT_LEN),
          f"y shape={y0.shape}, expected (1,1,{OUT_LEN}); "
          f"consumed = last {HOP} = output indices [{CONSUMED0}..{OUT_LEN-1}]")
    noise = rng.standard_normal((1, 1, N)).astype(np.float32)

    # b1: perturb every input before the consumed window -> consumed outputs
    # must be byte-identical.
    xp = x.copy()
    xp[..., :CONSUMED0] += 10.0 * noise[..., :CONSUMED0]
    y1 = run(sess, xp)
    ch = changed_indices(y0, y1)
    ch_consumed = ch[ch >= CONSUMED0]
    diff = float(np.max(np.abs(y1[..., -HOP:] - y0[..., -HOP:])))
    check(f"b1 perturb inputs [0..{CONSUMED0-1}]",
          ch_consumed.size == 0,
          f"changed consumed outputs: {fmt_set(ch_consumed)}, "
          f"max|diff| over consumed = {diff:.3e} "
          f"(non-consumed changed: {fmt_set(ch[ch < CONSUMED0])})")

    # b2: boundary probe -- perturb ONLY input CONSUMED0 (the oldest tap of
    # the first consumed output). Nominal expectation: only consumed output
    # CONSUMED0 changes. The oldest taps are empirically inert (effective RF
    # < declared RF), so an entirely-empty change set is also acceptable —
    # the rigorous non-vacuous dependency bound is b2b's scan below. What b2
    # must exclude is rightward leakage: any change past output CONSUMED0.
    def perturb_one(i, delta):
        xp = x.copy()
        xp[..., i] += delta
        return changed_indices(y0, run(sess, xp))

    ch = np.union1d(perturb_one(CONSUMED0, 10.0), perturb_one(CONSUMED0, -10.0))
    ch_consumed = ch[ch >= CONSUMED0]
    leak_ok = ch_consumed.size == 0 or ch_consumed.max() <= CONSUMED0
    check(f"b2 perturb input {CONSUMED0} only (+/-10)",
          leak_ok,
          f"changed consumed outputs: {fmt_set(ch_consumed)} (nominal "
          f"{{{CONSUMED0}}}; empty = inert oldest taps, bounded by b2b); "
          f"full-output changed: {fmt_set(ch)}; no rightward leakage past "
          f"output {CONSUMED0}: {leak_ok}")

    # b2b: boundary scan -- smallest single input index whose perturbation
    # (+/-10) changes ANY consumed output. Claim requires >= 394; a value
    # > 394 means the effective receptive field is < 631 (extra margin).
    first_affecting = None
    for i in range(max(0, CONSUMED0 - 3), CONSUMED0 + 10):
        ch_i = np.union1d(perturb_one(i, 10.0), perturb_one(i, -10.0))
        if np.any(ch_i >= CONSUMED0):
            first_affecting = i
            break
    check("b2b consumed-dependency boundary",
          first_affecting is not None and first_affecting >= CONSUMED0,
          f"first input index affecting any consumed output = {first_affecting} "
          f"(claim requires >= {CONSUMED0}; == {CONSUMED0} nominal; larger => "
          f"effective RF {RF - (first_affecting - CONSUMED0)} < {RF})")

    # b3: positive control 2 -- perturb only input 1500.
    # Nominal claim-expectation [870..1500] must be clipped: the last output
    # index is 1417 (output j depends on inputs [j..j+630] => input 1500
    # affects outputs [870..min(1500,1417)] = [870..1417]).
    i = 1500
    lo = max(i - (RF - 1), CONSUMED0)   # 870
    hi = min(i, OUT_LEN - 1)            # 1417
    ch = np.union1d(perturb_one(i, 10.0), perturb_one(i, -10.0))
    ch_consumed = ch[ch >= CONSUMED0]
    inside = (ch_consumed >= lo) & (ch_consumed <= hi)
    bounds_ok = (ch_consumed.size > 0 and inside.all()
                 and ch_consumed.min() == lo and ch_consumed.max() == hi)
    density = ch_consumed.size / (hi - lo + 1) if ch_consumed.size else 0.0
    check("b3 perturb input 1500 only (+/-10)",
          bounds_ok and density >= 0.95,
          f"changed consumed outputs: {fmt_set(ch_consumed)}, expected "
          f"[{lo}..{hi}] (nominal [870..1500] clipped to last output {OUT_LEN-1}); "
          f"density={density:.3f} (gaps = sub-float32-ULP responses); "
          f"consumed<{lo} identical={not np.any(ch_consumed < lo)}; "
          f">{hi} identical={not np.any(ch_consumed > hi)}")

    # ---------------- (c) TIMING ----------------
    for _ in range(20):
        run(sess, x)
    times = []
    for _ in range(200):
        t0 = time.perf_counter()
        run(sess, x)
        times.append((time.perf_counter() - t0) * 1e3)
    med = float(np.median(times))
    p95 = float(np.percentile(times, 95))
    lower = (1.0 - SHORT_T / N) * 100.0        # input-scaling bound: 19.2%
    upper = (1.0 - HOP / OUT_LEN) * 100.0      # output-scaling bound: 27.8%
    print(f"[INFO] timing [1,1,{N}] x200: median={med:.3f} ms  p95={p95:.3f} ms "
          f"(plugin sees ~2.1-2.4 ms hop-boundary blocks on this machine)")
    print(f"[INFO] analytic saving for T={SHORT_T}: lower(input-scaled) "
          f"1-{SHORT_T}/{N} = {lower:.1f}%, upper(output-scaled) "
          f"1-{HOP}/{OUT_LEN} = {upper:.1f}%; true value layer-dependent, in between")

    # Optional dynamic-axis probe (in-memory only; requires the onnx package).
    dyn_pct = None
    try:
        import onnx  # noqa: F401
    except ImportError:
        print("[INFO] dynamic probe SKIPPED: python 'onnx' package unavailable "
              "(pip install intentionally not attempted)")
    else:
        m = onnx.load(str(model_path))
        for vi in list(m.graph.input) + list(m.graph.output):
            d = vi.type.tensor_type.shape.dim[2]
            d.ClearField("dim_value")
            d.dim_param = "T"
        dsess = make_session(m.SerializeToString())
        yd = run(dsess, x)
        same_full = yd.tobytes() == y0.tobytes()
        yd_short = run(dsess, x[..., -SHORT_T:])
        same_tail = yd_short[..., -HOP:].tobytes() == y0[..., -HOP:].tobytes()
        for _ in range(20):
            run(dsess, x[..., -SHORT_T:])
        dtimes = []
        for _ in range(200):
            t0 = time.perf_counter()
            run(dsess, x[..., -SHORT_T:])
            dtimes.append((time.perf_counter() - t0) * 1e3)
        dmed = float(np.median(dtimes))
        dyn_pct = (1.0 - dmed / med) * 100.0
        check("c-dyn dynamic-axis probe", same_full and same_tail,
              f"T={N} byte-identical={same_full}; T={SHORT_T} last-{HOP} "
              f"byte-identical={same_tail}; median {dmed:.3f} ms vs {med:.3f} ms "
              f"=> measured speedup {dyn_pct:.1f}%")

    print(f"RESULT median_ms={med:.4f} p95_ms={p95:.4f} "
          f"saving_lower_pct={lower:.2f} saving_upper_pct={upper:.2f} "
          f"dynamic_probe_measured_pct={'%.2f' % dyn_pct if dyn_pct is not None else 'null'}")
    if failures:
        print(f"OVERALL: FAIL ({len(failures)} failed: {', '.join(failures)})")
        sys.exit(1)
    print("OVERALL: PASS")


if __name__ == "__main__":
    main()
