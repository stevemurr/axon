# Axon DSP Module Guide

Tutorial-style documentation for every DSP module in the Axon neural mastering
plugin. Each page starts from zero (no audio/DSP background assumed) and builds
up to the precise math and a line-referenced code walkthrough, so it's useful to
newcomers and veteran engineers alike.

## The signal chain

The default processing order (stages are reorderable in the UI):

```
input ─▶ Auto EQ ─▶ Saturator ─▶ SSL Comp ─▶ Bass Mono ─▶ Mel Limiter ─▶ True-Peak Ceiling ─▶ output
          (neural)   (neural)     (neural)                                                        │
                                                                                                  ▼
  in/out taps ─▶ Loudness Meter ─▶ Auto Gain (monitoring-only level-matched bypass)
  per-stage taps ─▶ Spectrum Analyzer (UI visualization)
```

## Processing stages (in chain order)

| # | Module | Doc | What it does |
|---|--------|-----|--------------|
| 1 | Auto EQ (Spectral-Mask EQ) | [auto_eq_spectral_mask.md](auto_eq_spectral_mask.md) | Neural, per-band STFT magnitude-mask EQ |
| 2 | Saturator / SSL Comp (neural engine) | [neural_inference_ort.md](neural_inference_ort.md) | ONNX-Runtime streaming TCN inference for learned saturation & compression |
| – | Rational-A nonlinearity | [rational_a.md](rational_a.md) | The learned rational waveshaper used inside the Saturator |
| 3 | Bass Mono | [bass_mono.md](bass_mono.md) | Collapses stereo to mono below a cutoff (LR4 on the side) |
| 4 | Mel Limiter | [mel_limiter.md](mel_limiter.md) | Mel-spaced multiband spectral limiter |
| 5 | True-Peak Ceiling | [true_peak_ceiling.md](true_peak_ceiling.md) | Final inter-sample (dBTP) safety limiter |

## Metering & monitoring

| Module | Doc | What it does |
|--------|-----|--------------|
| Loudness Meter | [loudness_meter.md](loudness_meter.md) | In/out LUFS + RMS + peak (BS.1770 K-weighting) |
| Auto Gain | [auto_gain.md](auto_gain.md) | Loudness-neutral, monitoring-only level-matched bypass |
| Spectrum Analyzer | [spectrum_analyzer.md](spectrum_analyzer.md) | Goertzel-based per-stage spectrum for the UI |

## Legacy / reference

| Module | Doc | Status |
|--------|-----|--------|
| LUFS Leveler | [lufs_leveler.md](lufs_leveler.md) | Not in the active chain — conceptual ancestor of the Loudness Meter + Auto Gain |

---

Source lives in [`../native/clap/src`](../native/clap/src). Each doc links back
to the exact `file:line` it describes.
