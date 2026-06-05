<div align="center">

# 🧠 Axon

### Adaptive **neural mastering** — an entire mastering chain in one CLAP plugin.

Differentiable DSP + learned controllers (LSTM/RNN) that listen to your mix and
adapt EQ, saturation, glue, width and loudness — in real time, inside your DAW.

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
adaptive interface. Some stages are **neural** (per-mix adaptive EQ, saturation,
SSL-style bus compression) running on ONNX Runtime; others are **hand-written
DSP** (LUFS levelers, a Mel-band loudness maximizer, bass mono-maker, true-peak
ceiling) accelerated with Apple vDSP. Drag to reorder, dial in, and read it all
on a live in/out LUFS / RMS / peak meter.

Built on [nablafx](https://github.com/stevemurr/nablafx) (our fork of
[mcomunita/nablafx](https://github.com/mcomunita/nablafx)).

## ✨ Highlights

- 🎚️ **Adaptive Auto-EQ** — an LSTM controller drives a 5-band parametric or a
  64-band spectral-mask EQ, with per-genre presets (bass / drums / vocals /
  other / full-mix).
- 🔥 **Neural saturation & SSL bus glue** — rational-activation soft-clipper and
  a TCN-emulated SSL-style bus compressor.
- 📣 **Loudness maximizer** — 26-band Mel STFT limiter with reverse
  water-filling, a Drive control, and a true-peak lookahead brickwall
  (0.1 % THD vs 22.7 % for a clipper on bass — see the
  [deep dive](native/clap/docs/limiter_algorithm.md)).
- 🎛️ **Bass mono-maker** — collapse the image to mono below a cutoff (default
  250 Hz) for a tight, translatable low end; the mono sum is preserved exactly.
- 📊 **Live metering** — in/out **LUFS** (short-term + momentary), **RMS** and
  **peak**, with a −14…−11 LUFS streaming-target zone.
- 🧩 **Fully reorderable chain** — drag stages into any order; latency is
  reported to the host for sample-accurate delay compensation.

## 🎛️ The chain

Default order (drag to reorder; Input/Output levelers are pinned first/last,
True-Peak Ceiling is always final):

| # | Stage | What it does | Key controls |
|---|-------|--------------|--------------|
| 1 | **Input Leveler** | LUFS-matched input gain toward a target | `Leveler`, `Lev Target` |
| 2 | **Auto EQ** 🧠 | Per-class adaptive EQ (5-band parametric **or** 64-band spectral mask) driven by an LSTM | `EQ`, `Class`, `Range`, `Boost`, `Speed` |
| 3 | **Saturator** 🧠 | Rational soft-clipper with HPF/LPF, threshold & bias | `Drive`, `Output`, `Mix`, `Thresh`, `Bias` |
| 4 | **Bus Comp** 🧠 | TCN-emulated SSL-style bus compressor (on/off) | `Bus Comp` |
| 5 | **Bass Mono** | Mono below a cutoff; mono sum preserved exactly | `Bass Mono` (on/off), `Frequency` |
| 6 | **Limiter** | Mel-band maximizer + true-peak lookahead brickwall | `Drive`, `Ceiling`, `Attack/Adaptive Gain`, `Release/Adaptive Speed`, `Dynamic` |
| 7 | **Output Leveler** | LUFS leveling at the output | `Out Lev`, `Out Target` |
| — | **True-Peak Ceiling** | Always-last 4× oversampled brickwall, guarantees the dBTP ceiling | (fixed) |

🧠 = neural (ONNX); the rest is native DSP.

## 🚀 Quick start (macOS, Apple Silicon)

```sh
git clone https://github.com/stevemurr/axon
cd axon
bash scripts/install_axon_mac.sh
# Restart your DAW (or rescan plugins) → load "Axon" from the CLAP list.
```

The installer stages the shipped model bundle (`weights/axon_bundle/`), builds
the `.clap`, code-signs it, and installs to
`~/Library/Audio/Plug-Ins/CLAP/Axon.clap`.

## 🔬 Inside the limiter

The limiter is the most novel piece — a two-domain design: a **frequency-domain
26-band Mel maximizer** (reverse water-filling that only ducks the offending
bands) feeding a **time-domain true-peak lookahead brickwall** (256-sample
sliding-window peak detector + hard safety clip). A **Drive** knob pushes
loudness into a true **Ceiling**, and a **Dynamic** toggle routes the adaptive
controls into the brickwall's attack/release for an Elevate-like, breathing
character. Full walkthrough with the math:

📖 **[native/clap/docs/limiter_algorithm.md](native/clap/docs/limiter_algorithm.md)**

## 🛠️ Building from source

```sh
cd native/clap
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target axon_clap
```

Requires macOS arm64, CMake, and the Xcode command-line tools. ONNX Runtime is
fetched automatically. `scripts/install_axon_mac.sh` wraps staging + build +
install for end users.

## 🧪 Tests

The native DSP has standalone unit tests (no external deps):

```sh
cd native/clap
cmake --build build --target test_mel_limiter test_meter test_bass_mono
build/test_mel_limiter   # 13 units: WOLA reconstruction, ceiling, drive, lookahead THD…
build/test_meter         # LUFS cross-checked against the reference leveler
build/test_bass_mono     # mono-below-cutoff, exact mono-sum preservation
```

## 🏋️ Training new Auto-EQ models

```sh
# 1. Prep an augmented per-class dataset (e.g. full_mix) from MUSDB:
uv run python scripts/prepare_auto_eq_data.py --musdb \
    --src /path/to/musdb18 --target-class full_mix --augment-pre-eq \
    --max-trainval 800 --max-test 80 \
    --out /path/to/datasets/axon_auto_eq_musdb_full_mix_aug

# 2. Train (spectral-mask 64-band config by default):
uv run nablafx \
    data=auto_eq_musdb_full_mix_aug_trainval \
    model=gb/tone_auto_eq/model_gb_tone_auto_eq_spectral_mask_2048_musdb.d \
    trainer=gb max_steps=2000

# 3. Verify the controller actually adapts:
uv run python scripts/probe_auto_eq_adaptivity.py --run-dir <hydra_run>

# 4. Export the bundle into the composite staging dir:
uv run nablafx-export --run-dir <hydra_run> --out weights/axon_bundle/auto_eq_full_mix
```

## 📁 Repo layout

```
axon/                       (repo root)
├── axon/                   Python package — export/composite.py composes bundles
├── native/clap/            C++ CLAP plugin (macOS arm64)
│   ├── src/                runtime, DSP blocks (mel_limiter, meter, bass_mono…), ORT session
│   ├── ui/                 WebKit GUI (index.html)
│   ├── tests/              standalone DSP unit tests
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
- **Native DSP:** LUFS levelers, `MelLimiter`, `BassMono`, `LoudnessMeter`,
  `TruePeakCeiling`, `SpectralMaskEQ` — all dependency-free and unit-tested.
- **Bundle format:** `model.onnx` + `plugin_meta.json` (+ source Hydra yaml) per
  stage, composed by `axon_meta.json`.
- **Why a nablafx fork:** adds `SpectralMaskEQ` / `SpectralDynamicController` and
  the `dynamic-spectral` control type. Required until those land upstream.

## 🙏 Credits

Built on [nablafx](https://github.com/mcomunita/nablafx) by Marco Comunità.
Plugin format: [CLAP](https://cleveraudio.org/).

## 📄 License

No license file is currently included — all rights reserved by the author
pending a license decision. Contact the maintainer for usage terms.
