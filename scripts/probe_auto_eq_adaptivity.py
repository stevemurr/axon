#!/usr/bin/env python3
"""Adaptivity probe for the Auto-EQ neural controllers (64-band contract).

Answers ONE question: does the controller genuinely adapt to program
material, or has it mode-collapsed to a static curve? (The 2026-06 5-band
PEQ controller collapsed; the shipped 64-band models were measured adaptive
on 2026-07-06 — this script makes that measurement repeatable.)

Two modes:

  BUNDLE (default; runs anywhere with onnxruntime): probes the SHIPPED
  ONNX bundles, replicating the plugin runtime contract exactly — peak-hold
  envelope preprocessing (kEnvTauSeconds=0.5, axon_plugin.cpp:1090),
  state-carried block-by-block inference, timestep-0 param read,
  params->dB via the class meta's gain span. Shape-driven: handles both
  batch-1 (fresh nablafx-export output) and batch-2 (shipped, post
  2026-07-05 surgery) models.

    uv run axon autoeq probe                    # all shipped classes
    uv run axon autoeq probe --class full_mix
    uv run axon autoeq probe --bundle path/to/auto_eq_x

  RUN-DIR (training host; needs torch/nablafx): probes a hydra training run
  BEFORE export, reading the controller's full param vector.

    uv run axon autoeq probe --run-dir <hydra_run>

Verdict thresholds (across-material spread of the settled curve):
ADAPTIVE >= 3 dB, WEAK 1-3 dB, COLLAPSED < 1 dB. Exit 1 on COLLAPSED.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[1]
BLK = 128
ENV_TAU_S = 0.5          # kEnvTauSeconds — axon_plugin.cpp:1090
WARMUP_S = 2.0


# ---------------------------------------------------------------- materials

def _peak_norm(x: np.ndarray, dbfs: float) -> np.ndarray:
    p = float(np.max(np.abs(x))) + 1e-12
    return (x * (10.0 ** (dbfs / 20.0) / p)).astype(np.float32)


def _colored_noise(n: int, sr: int, beta: float, seed: int) -> np.ndarray:
    """Amplitude spectrum ~ f^(-beta/2): beta=1 pink, 2 dark, -1 bright."""
    rng = np.random.default_rng(seed)
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, d=1.0 / sr)
    f[0] = f[1]
    return np.fft.irfft(spec * f ** (-beta / 2.0), n=n).astype(np.float32)


def build_materials(sr: int, seconds: float, wav: str | None) -> dict[str, np.ndarray]:
    n = int(sr * seconds)
    mats = {
        "pink":   _peak_norm(_colored_noise(n, sr, 1.0, 1), -12.0),
        "dark":   _peak_norm(_colored_noise(n, sr, 2.0, 2), -12.0),
        "bright": _peak_norm(_colored_noise(n, sr, -1.0, 3), -12.0),
        "quiet_pink": _peak_norm(_colored_noise(n, sr, 1.0, 4), -30.0),
        "loud_pink":  _peak_norm(_colored_noise(n, sr, 1.0, 4), -6.0),
    }
    # Non-stationary: dark <-> bright flip every 0.5 s.
    seg = int(sr * 0.5)
    d = _peak_norm(_colored_noise(n, sr, 2.0, 5), -12.0)
    b = _peak_norm(_colored_noise(n, sr, -1.0, 6), -12.0)
    ns = np.empty(n, dtype=np.float32)
    for k in range(0, n, seg):
        j = min(k + seg, n)
        ns[k:j] = (d if (k // seg) % 2 == 0 else b)[k:j]
    mats["nonstationary"] = ns
    if wav:
        import wave
        import struct as _s
        with wave.open(wav, "rb") as w:
            raw = w.readframes(w.getnframes())
            ch = w.getnchannels()
            x = np.array(_s.unpack(f"<{len(raw)//2}h", raw), dtype=np.float32) / 32768.0
            mats["wav"] = x[::ch][:n]
    return mats


# ---------------------------------------------------------------- runners

def make_onnx_runner(bundle: Path):
    """Contract-exact runner over a shipped bundle dir. Returns
    (step(block)->params01[n_params], reset(), info dict)."""
    import onnxruntime as ort_rt
    meta = json.loads((bundle / "plugin_meta.json").read_text())
    p = (meta.get("dsp_blocks") or [{}])[0].get("params", {})
    info = {
        "sr": int(p.get("sample_rate", meta.get("sample_rate", 44100))),
        "n_bands": int(p.get("n_bands", 64)),
        "min_db": float(p.get("min_gain_db", -18.0)),
        "max_db": float(p.get("max_gain_db", 18.0)),
        "f_min": float(p.get("f_min", 30.0)),
        "f_max": float(p.get("f_max", 22050.0)),
    }
    so = ort_rt.SessionOptions()
    so.intra_op_num_threads = 1
    so.inter_op_num_threads = 1
    so.graph_optimization_level = ort_rt.GraphOptimizationLevel.ORT_ENABLE_ALL
    sess = ort_rt.InferenceSession(str(bundle / "model.onnx"), so,
                                   providers=["CPUExecutionProvider"])
    audio_name = meta["input_names"][0]
    batch = int(sess.get_inputs()[0].shape[0])      # 1 (export) or 2 (shipped)
    states = meta.get("state_tensors") or []
    out_names = [meta["output_names"][0]] + [s["name"] + "_out" for s in states]
    state = {}

    def reset():
        for s in states:
            state[s["name"]] = np.zeros(s["shape"], np.float32)
    reset()

    def step(ctrl_block: np.ndarray) -> np.ndarray:
        feed = {audio_name: np.tile(ctrl_block.reshape(1, 1, BLK), (batch, 1, 1))}
        for s in states:
            feed[s["name"] + "_in"] = state[s["name"]]
        outs = sess.run(out_names, feed)
        for i, s in enumerate(states):
            state[s["name"]] = outs[1 + i]
        return outs[0][0, :, 0]                     # batch elem 0, timestep 0

    return step, reset, info


def make_torch_runner(run_dir: Path, ckpt: str | None):
    """Runner over a hydra training run (training host only)."""
    try:
        import torch
        from nablafx.export.bundle import _load_system_and_weights  # type: ignore
    except ImportError:
        print("error: --run-dir mode needs torch + nablafx — run on the "
              "training host after `uv sync --extra train`.", file=sys.stderr)
        raise SystemExit(2)
    system = _load_system_and_weights(
        run_dir.resolve(), ckpt_path=Path(ckpt) if ckpt else None)
    ctrl = system.model.controller.controllers[0]
    ctrl.eval()
    info = {"sr": 44100, "n_bands": None, "min_db": -18.0, "max_db": 18.0,
            "f_min": 30.0, "f_max": 22050.0}

    def reset():
        if hasattr(ctrl, "reset_state"):
            ctrl.reset_state()
    reset()

    def step(ctrl_block: np.ndarray) -> np.ndarray:
        with torch.no_grad():
            params = ctrl(torch.from_numpy(ctrl_block).view(1, 1, BLK))
        v = params[0, :, 0].numpy()
        if info["n_bands"] is None:
            info["n_bands"] = v.shape[0]
        return v

    return step, reset, info


# ---------------------------------------------------------------- probe

def run_material(step, reset, x: np.ndarray, span: tuple[float, float]) -> np.ndarray:
    """Envelope-normalize + run block-by-block; return per-block dB curves."""
    reset()
    sr_blocks = len(x) // BLK
    env_decay = math.exp(-1.0 / max((44100 * ENV_TAU_S) / BLK, 1.0))
    env = 0.0
    curves = None
    for bi in range(sr_blocks):
        blk = x[bi * BLK:(bi + 1) * BLK]
        pk = float(np.max(np.abs(blk)))
        env = pk if pk > env else env_decay * env + (1.0 - env_decay) * pk
        scale = (0.5 / env) if env > 1e-6 else 1.0
        g = step((blk * scale).astype(np.float32))
        if curves is None:
            curves = np.empty((sr_blocks, g.shape[0]), np.float32)
        curves[bi] = span[0] + g * (span[1] - span[0])
    return curves


def probe(step, reset, info, mats, label: str) -> tuple[str, float]:
    warm = int(math.ceil(WARMUP_S * 44100 / BLK))
    settled, within_p2p = {}, {}
    for name, x in mats.items():
        c = run_material(step, reset, x, (info["min_db"], info["max_db"]))[warm:]
        settled[name] = c.mean(axis=0)
        within_p2p[name] = float((c.max(axis=0) - c.min(axis=0)).max())
    stack = np.stack(list(settled.values()))
    spread_band = stack.max(axis=0) - stack.min(axis=0)
    spread = float(spread_band.max())
    worst = int(np.argmax(spread_band))
    verdict = "ADAPTIVE" if spread >= 3.0 else ("WEAK" if spread >= 1.0 else "COLLAPSED")

    print(f"\n== {label}: n_bands={stack.shape[1]}, "
          f"span [{info['min_db']:.0f}, {info['max_db']:.0f}] dB")
    print(f"{'material':<16} {'mean dB (first/mid/last band)':<34} within p2p dB")
    for name, m in settled.items():
        tri = f"{m[0]:+6.2f} / {m[len(m)//2]:+6.2f} / {m[-1]:+6.2f}"
        print(f"{name:<16} {tri:<34} {within_p2p[name]:6.2f}")
    print(f"across-material spread: {spread:.2f} dB (worst band index {worst}) "
          f"-> {verdict}")
    print(f"RESULT label={label} spread_db={spread:.3f} verdict={verdict}")
    return verdict, spread


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--class", dest="cls", default="all",
                     help="shipped class name or 'all' (default)")
    src.add_argument("--bundle", help="path to one auto_eq_<class> bundle dir")
    src.add_argument("--run-dir", help="hydra training run (training host)")
    ap.add_argument("--ckpt", default=None, help="explicit ckpt (with --run-dir)")
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--wav", default=None, help="optional extra material (16-bit wav)")
    args = ap.parse_args()

    targets: list[tuple[str, object]] = []
    if args.run_dir:
        targets.append((f"run:{Path(args.run_dir).name}",
                        make_torch_runner(Path(args.run_dir), args.ckpt)))
    elif args.bundle:
        b = Path(args.bundle)
        targets.append((b.name, make_onnx_runner(b)))
    else:
        root = REPO / "weights" / "axon_bundle"
        names = ([args.cls] if args.cls != "all" else
                 sorted(d.name.removeprefix("auto_eq_")
                        for d in root.glob("auto_eq_*")))
        for c in names:
            targets.append((c, make_onnx_runner(root / f"auto_eq_{c}")))

    worst_verdict = "ADAPTIVE"
    for label, (step, reset, info) in targets:
        mats = build_materials(44100, args.seconds, args.wav)
        verdict, _ = probe(step, reset, info, mats, label)
        if verdict == "COLLAPSED" or (verdict == "WEAK" and worst_verdict == "ADAPTIVE"):
            worst_verdict = verdict

    print(f"\nOVERALL: {worst_verdict}")
    return 1 if worst_verdict == "COLLAPSED" else 0


if __name__ == "__main__":
    raise SystemExit(main())
