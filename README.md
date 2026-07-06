<div align="center">

# 🧠 Axon

### Adaptive **neural mastering** — an entire mastering chain in one CLAP plugin.

Differentiable DSP + learned controllers that listen to your mix and adapt EQ,
glue, space, width and loudness — in real time, inside your DAW.

![platform](https://img.shields.io/badge/platform-macOS%20arm64-111111)
![format](https://img.shields.io/badge/plugin-CLAP-e76f51)
![language](https://img.shields.io/badge/C%2B%2B-17-5fa8d3)
![runtime](https://img.shields.io/badge/inference-ONNX%20Runtime-005CED)
![dsp](https://img.shields.io/badge/DSP-Accelerate%20vDSP-2a9d8f)
![status](https://img.shields.io/badge/status-active%20development-e9c46a)

<img src="demo.png" alt="Axon plugin UI" width="760">

</div>

---

Axon packs a complete, reorderable mastering signal path behind a single,
adaptive interface. Some stages are **neural** — a per-mix adaptive Auto-EQ and a
TCN bus compressor running on ONNX Runtime; the rest are **hand-written DSP** — a
parametric channel EQ, an FDN room reverb, an M/S stereo widener, a Mel-band
loudness maximizer, a bass mono-maker and a true-peak ceiling — accelerated with
Apple vDSP. Drag to reorder, dial in, and read it all on a live in/out
LUFS / RMS / peak meter plus an EQ overlay that shows the total curve and each
EQ stage's contribution.

Built on [nablafx](https://github.com/stevemurr/nablafx) (our fork of
[mcomunita/nablafx](https://github.com/mcomunita/nablafx)).

## ✨ Highlights

- 🎚️ **Adaptive Auto-EQ** — a learned controller (a neural LSTM **or** a
  deterministic adaptive cascade) drives a per-class corrective EQ, rendered by a
  **zero-latency minimum-phase IIR filterbank** (the default) or a 64-band STFT
  spectral mask. Per-class presets: bass / drums / vocals / other / full-mix.
- 🎛️ **Parametric channel EQ** — a broadband LF / LMF / HMF / HF EQ (shelves and
  bells, switchable) with HPF/LPF and a harmonic Colour control. It can
  **calibrate itself against the Auto-EQ**: press **Recalibrate** and it voices
  its bands toward the adaptive correction with broad, musical moves, leaving the
  Auto-EQ to handle the fine detail; **Reset** clears it.
- 🧠 **Neural bus glue** — a TCN-emulated bus compressor with an `Input` drive
  that sets the model's operating point (level-matched, so it stays
  loudness-neutral while you push it into its sweet spot).
- 🌌 **Room reverb & stereo widener** — a transparent 8-line FDN room for subtle
  depth, and a mono-safe frequency-dependent M/S "shuffler" for width.
- 📣 **Loudness maximizer** — 26-band Mel STFT limiter with reverse
  water-filling, a Drive control, and a true-peak lookahead brickwall
  (0.1 % THD vs 22.7 % for a clipper on bass — see the
  [deep dive](native/clap/docs/limiter_algorithm.md)).
- 🎛️ **Bass mono-maker** — collapse the image to mono below a cutoff (default
  ~225 Hz) for a tight, translatable low end; the mono sum is preserved exactly.
- 📊 **Live metering** — in/out **LUFS** (short-term + momentary), **RMS** and
  **peak**, with a −14…−11 LUFS streaming-target zone.
- 🧩 **Fully reorderable chain** — drag stages into any order; latency is reported
  to the host for sample-accurate delay compensation.

## 🎛️ The chain

Default order (drag to reorder; the True-Peak Ceiling is always final):

| # | Stage | What it does | Key controls |
|---|-------|--------------|--------------|
| 1 | **Bass Mono** | Mono below a cutoff; mono sum preserved exactly | `Bass Mono` (on/off), `Frequency` |
| 2 | **EQ** | Broadband parametric channel EQ — LF/LMF/HMF/HF (shelf↔bell), HPF/LPF and a harmonic Colour; can calibrate against the Auto-EQ | `EQ` (on/off), per-band `Gain`/`Freq`/`Q`, `HPF`/`LPF`, `Colour`, `Auto Assist`, `Split`, `Recalibrate`, `Reset` |
| 3 | **Auto EQ** 🧠 | Per-class adaptive corrective EQ; neural **or** deterministic engine; zero-latency IIR **or** STFT renderer | `Auto EQ`, `Class`, `Range`, `Boost`, `Speed`, `Engine`, `Renderer` |
| 4 | **Reverb** | Transparent 8-line FDN room (bass-excluded, damped, mono-compatible) | `Mix`, `Size`, `Width`, `Damp`, `Low Cut` |
| 5 | **Widener** | Frequency-dependent M/S "shuffler" — wider mids/highs, mono sum invariant | `Width` (on/off), `Amount`, `Low`, `Air` |
| 6 | **Bus Comp** 🧠 | TCN-emulated bus compressor with a level-matched input drive | `Bus Comp` (on/off), `Input` |
| 7 | **Limiter** | Mel-band maximizer + true-peak lookahead brickwall | `Drive`, `Ceiling`, `Attack/Adaptive Gain`, `Release/Adaptive Speed`, `Dynamic` |
| — | **True-Peak Ceiling** | Always-last 4× oversampled brickwall, guarantees the dBTP ceiling | (fixed) |

🧠 = neural (ONNX Runtime); the rest is native DSP. A neural **Saturator**
remains in the codebase but is not in the current default chain (re-enable by
putting its id back in the order).

## 🚀 Quick start (macOS, Apple Silicon)

```sh
git clone https://github.com/stevemurr/axon
cd axon
uv run axon install --mac
# Restart your DAW (or rescan plugins) → load "Axon" from the CLAP list.
```

This builds the `.clap` from the committed model bundle (`weights/axon_bundle/`),
ad-hoc code-signs it, and installs to `~/Library/Audio/Plug-Ins/CLAP/Axon.clap`
(under the hood: `scripts/install_axon_mac.sh`). No Python dependencies are
pulled — the CLI is a thin, dependency-free router.

## 🧰 One CLI for everything

Every dev flow has a single entrypoint via [uv](https://docs.astral.sh/uv/) —
each subcommand is a thin wrapper that delegates to the canonical script it
names (documented in the sections below), so there is exactly one
implementation of each flow:

```sh
uv run axon build                 # build the .clap (Release-guarded)
uv run axon build --instrumented  # bench-only per-stage-timing build (never installable)
uv run axon install --mac         # build + install into ~/Library/Audio/Plug-Ins/CLAP
uv run axon test                  # build, then ctest (full suite)
uv run axon bench                 # scenario × buffer matrix (bench/run_bench.py args pass through)
uv run axon coverage              # llvm-cov over the test suite
uv run axon eval null             # A/B null: current tree vs the installed plugin
uv run axon eval ssl-comp         # ssl_comp model sizing invariants
uv run axon train                 # auto-EQ fan-out (GPU host; needs `uv sync --extra train`)
```

`eval null` encodes the null-test protocol (data-chunk-only compares — wav
metadata embeds timestamps — plus the retry rule for the known ORT run-to-run
nondeterminism; see `native/clap/docs/investigations/`). The local CLI is
dependency-free; only training pulls torch, behind the `train` extra.

## 🔬 Inside the limiter

The limiter is the most novel piece — a two-domain design: a **frequency-domain
26-band Mel maximizer** (reverse water-filling that only ducks the offending
bands) feeding a **time-domain true-peak lookahead brickwall** (256-sample
sliding-window peak detector + hard safety clip). A **Drive** knob pushes
loudness into a true **Ceiling**, and a **Dynamic** toggle routes the adaptive
controls into the brickwall's attack/release for a breathing character. Full
walkthrough with the math:

📖 **[native/clap/docs/limiter_algorithm.md](native/clap/docs/limiter_algorithm.md)**

## 🛠️ Building from source

```sh
uv run axon build                 # build the .clap (no install)
uv run axon build --instrumented  # bench-only per-stage-timing build (never installable)
uv run axon install --mac         # build + install
```

Requires macOS arm64, CMake, and the Xcode command-line tools. ONNX Runtime is
fetched automatically (SHA256-pinned). The committed `weights/axon_bundle/` is
the authoritative control set — build directly from it (don't regenerate the
meta from the training Python). Under the hood the CLI drives
`scripts/install_axon_mac.sh` → `native/clap/build.sh`, which carry the safety
guards (Release-only caches, no instrumented builds in the DAW folder); going
through the CLI keeps you inside those guards.

## 🧪 Tests

Standalone, dependency-free unit tests cover the DSP and the plugin contract:

```sh
uv run axon test              # build, then run the full suite
uv run axon test -- -R meter  # pass ctest args through (filter by name)
uv run axon coverage          # llvm-cov line coverage over the suite
```

(Equivalent underlying commands: `cmake --build native/clap/build` then
`ctest --test-dir native/clap/build --output-on-failure`.)

CTest only runs registered, freshly-built targets — unlike a `for t in
build/test_*` glob, it can't silently keep running a stale binary whose target
was deleted or renamed. It also runs `tests/test_ssl_integration.py` (skipped
automatically unless `build/Axon.clap` + `axon_bench` are built), and
`build.sh` re-runs the two fast meta↔plugin contract guards on every build.

They cover the limiter (WOLA reconstruction, ceiling, drive, lookahead THD), the
meter (LUFS cross-checked against a reference BS.1770 meter), bass mono, the EQ
engine + its calibration solver, the IIR / spectral-mask renderers, the adaptive
Auto-EQ controller, and the **meta↔plugin control contract** (the shipped
`axon_meta.json` control set must exactly match what the plugin reads). A
real-plugin integration check drives the built `.clap` through a headless CLAP
host to confirm end-to-end behaviour.

## 🏋️ Training new Auto-EQ models

Training runs on the GPU host and needs the `train` extra (pulls torch via the
pinned nablafx fork): `uv sync --extra train`.

```sh
# 1. Prep an augmented per-class dataset (e.g. full_mix) from MUSDB:
uv run python scripts/prepare_auto_eq_data.py --musdb \
    --src /path/to/musdb18 --target-class full_mix --augment-pre-eq \
    --out /path/to/datasets/axon_auto_eq_musdb_full_mix_aug

# 2. Train all five classes (spectral-mask 64-band config):
uv run axon train

# 3. Verify the controller actually adapts:
uv run python scripts/probe_auto_eq_adaptivity.py --run-dir <hydra_run>

# 4. Export the per-class bundle into the staging dir:
uv run nablafx-export --run-dir <hydra_run> --out weights/axon_bundle/auto_eq_full_mix
```

## 📁 Repo layout

```
axon/                       (repo root)
├── axon/                   Python package — export/composite.py composes bundles
├── native/clap/            C++ CLAP plugin (macOS arm64)
│   ├── src/                runtime, DSP blocks (mel_limiter, meter, bass_mono,
│   │                       reverb, widener, iir_filterbank_eq, adaptive_eq…), ORT session
│   ├── ui/                 WebKit GUI (index.html)
│   ├── tests/              standalone DSP + contract unit tests
│   ├── bench/              headless benchmarking harness
│   └── docs/               algorithm write-ups
├── conf/                   Hydra data + model configs (training)
├── scripts/                data prep, training fanout, export, build/install
├── weights/
│   ├── axon_bundle/        shipped per-stage bundles (model.onnx + meta) + axon_meta.json
│   └── auto_eq_refs/       per-class long-term spectrum references
├── docs/                   project notes
└── demo.png
```

## 🧱 Architecture notes

- **Inference:** ONNX Runtime via a thin `OrtMiniSession`; everything latency-
  and realtime-aware. FFTs use Apple Accelerate vDSP.
- **Native DSP (header-only, unit-tested):** `MelLimiter`, `BassMono`,
  `LoudnessMeter`, `TruePeakCeiling`, `SpectralMaskEq`, `IirFilterbankEq`,
  `AdaptiveEqController`, the parametric channel EQ + its coupling solver, reverb
  and widener.
- **Bundle format:** `model.onnx` + `plugin_meta.json` (+ source Hydra yaml) per
  stage, composed by `axon_meta.json` (the shipped, authoritative control set).
- **Why a nablafx fork:** adds `SpectralMaskEQ` / `SpectralDynamicController` and
  the `dynamic-spectral` control type. Required until those land upstream.

## 🙏 Credits

Built on [nablafx](https://github.com/mcomunita/nablafx) by Marco Comunità.
Plugin format: [CLAP](https://cleveraudio.org/).

## 📄 License

No license file is currently included — all rights reserved by the author pending
a license decision. Contact the maintainer for usage terms.
