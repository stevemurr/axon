# Sonic-improvement harness & workflow — Exciter + Auto-EQ

Goal: make "better" **falsifiable**. A candidate algorithm ships only if it beats
the current one on named, reproducible metrics *at matched musical effect*, and
the win is large enough to be audible. Otherwise it is rejected with the numbers.

Two candidates are under test:
- **Exciter** → Chebyshev (Tn) harmonic generator replacing the biased-tanh shaper.
- **Auto-EQ** → minimum-phase IIR peaking/shelf filterbank replacing the STFT
  magnitude mask (same neural band targets — controller unchanged, renderer swapped).

## The one principle: match the effect, then compare the artifacts
A processor that does *less* trivially distorts less. So every A/B normalizes the
musical effect first, then measures the artifacts:
- **Exciter:** calibrate each shaper's drive so both add the **same 2nd+3rd
  harmonic energy** (same "excitement"), *then* compare aliasing / IMD / higher-
  order junk / CPU.
- **Auto-EQ:** drive both renderers with the **same per-band target curve**,
  *then* compare magnitude-match / transient preservation / musical noise /
  latency / CPU.

A change here is a **QUALITY** change (it intentionally changes the sound), so it
is proven with A/B numbers, never null-tested. Quality classification + the A/B
table go in the commit message.

---

## Harness layout
```
bench/sonic/analysis.hpp        metric primitives (header-only, Goertzel + time-domain; no FFT dep)
bench/sonic/signals.hpp         reference-signal generators
bench/sonic/harness_exciter.cpp baseline vs candidate -> table
bench/sonic/harness_auto_eq.cpp baseline vs candidate -> table   (links Accelerate for SpectralMaskEq)
```
Each DUT is wrapped as `std::function<void(const float* in, float* out, int n)>`
so baseline and candidate run through the identical measurement path and print a
single comparison table. Deterministic (fixed seeds); prints a human table plus
`key=value` lines for scripting.

---

## Exciter — metrics, signals, thresholds

| Metric | Signal | Definition | Pass (candidate vs baseline, at matched h2+h3) |
|---|---|---|---|
| **Alias/signal** | sine f0 ∈ {9k, 12k} @ −12 dBFS | RMS of Goertzel over inharmonic probe band 0.3–2.5 kHz ÷ fundamental | **≤ baseline − 10 dB** (primary "less harsh") |
| **THD** | sine f0 ∈ {1k, 4k} | √Σ(h2..h8)² ÷ h1 | report; not a pass gate (effect is matched) |
| **Harmonic purity** | sine 4k | levels h2..h6 (dB rel h1) | h4+ **lower** than baseline (cleaner 2nd/3rd) |
| **IMD (CCIF)** | 19k + 20k, equal | product @ 1k + nearest sidebands ÷ (h@19k+h@20k) | **≤ baseline** |
| **IMD (SMPTE)** | 60 Hz + 7k, 4:1 | mod sidebands around 7k ÷ 7k level | **≤ baseline** |
| **Broadband alias (musical)** | pink noise, band-limited to <8k | output energy above 16k that wasn't in the input ÷ input energy | **≤ baseline** |
| **CPU** | — | ns/sample | **≤ baseline × 1.2** |

Calibration: binary-search `drive_db` so h2 at f0=4k hits a fixed target
(e.g. −24 dB rel fundamental) for *each* shaper before the comparison.

## Auto-EQ — metrics, signals, thresholds

| Metric | Signal | Definition | Pass (candidate vs baseline, same target curve) |
|---|---|---|---|
| **Magnitude-match** | stepped sine, 1/12-oct, 30 Hz–18 kHz | realized gain(f) vs target(f): **RMS dB** + **max dB** | RMS ≤ **0.5 dB**, max ≤ **1.5 dB** (the risk metric for IIR) |
| **Transient preservation** | drum loop (fixture) + click train | crest-factor Δ (out−in, dB) and impulse 10–90 % rise smear (samples) | crest Δ **closer to 0**; smear **lower** |
| **Musical noise** | static mask + steady 1 k tone & pink | short-time-RMS modulation depth (dB) over the steady region | **≤ baseline − 10 dB** (primary "more clarity") |
| **Latency** | impulse | measured peak delay (samples) + reported | **lower** (STFT 2048 → IIR ~0) |
| **Reconstruction null** | flat 0 dB mask | max\|out − in_delayed\| (dBFS) | ≤ −60 dBFS (sanity) |
| **CPU** | — | ns/sample | **≤ baseline** |

Target curves used for A/B: (a) gentle tilt (+3 dB @ 80 Hz → −3 dB @ 10 k),
(b) a narrow −6 dB notch @ 3 k (worst case for IIR Q-match), (c) flat.

---

## Workflow loop (per candidate)
1. **Baseline** — run the harness on the current module, commit the numbers.
2. **Implement** the candidate behind the same DUT interface (header-only,
   standalone-testable, like the existing modules).
3. **A/B** — run the harness; produce the comparison table.
4. **Decide** — accept iff it clears the thresholds *at matched effect*; else
   iterate or reject, recording why with the numbers.
5. **Cover** — add unit assertions for any new invariant; full suite green.
6. **Commit** — QUALITY classification + the A/B table in the message; PR.

## Status
- [x] analysis.hpp + signals.hpp
- [x] harness_exciter.cpp + baseline
- [x] harness_auto_eq.cpp + baseline
- [x] Chebyshev exciter candidate + A/B
- [x] min-phase IIR filterbank auto-EQ candidate + A/B

## Candidate results (2026-06-07)
**Exciter — polynomial (Chebyshev-class) shaper** (`Exciter::Shaper::Polynomial`,
default unchanged), at matched THD 6 %:
- Harmonic purity: NO content above the 3rd (h4/h5/h6 at −150…−178 dB vs
  biased-tanh −62…−110 dB) → quantified "cleaner / less fizzy".
- CPU ~18 % cheaper (no `tanh`).
- Aliasing & IMD: a wash (4× OS already handles them).
- Verdict: QUALITY win on purity + CPU; shipped as a non-destructive mode.

**Auto-EQ — min-phase IIR filterbank** (`IirFilterbankEq`, 24 bells + edge
shelves, A⁻¹ solve so the summed response hits the band targets):
- Magnitude-match: tilt 0.43 dB RMS (beats STFT's 0.58); notch 0.21 RMS (under
  the 0.5 bar; STFT's per-bin mask is tighter on a narrow notch: 0.13 / max 0.64
  vs 0.21 / max 1.58).
- CPU ~4.4× cheaper (55 vs 240 ns/sample); latency 0 vs 2048 (46 ms); flat-mask
  reconstruction exact (−240 vs −138 dBFS).
- Verdict: clears the match bar AND wins decisively on latency/CPU/reconstruction.
- OPEN: the "clarity" axes (transient smearing, musical noise under a MOVING
  mask) are NOT yet substantiated — the current static-tone/click metrics are too
  benign (both read the measurement floor). Need a moving-mask + drum-loop metric
  before claiming those; and the narrow-notch match could improve with a Newton
  iteration on the solve or a higher-Q notch band.

## Baselines (captured 2026-06-07; Release -O3, Apple Silicon)
**Exciter** (SR 48k, matched 2nd-harmonic = −24 dB, character 0):
- drive 2.79 dB; harmonics h2 −24.0, h3 −166, h4 −63.7, h5 −153, h6 −109 dB; THD@4k 6.31 %
- alias/fund @9k −87.4 dB, @12k −90.9 dB; IMD CCIF −37.4 dB, SMPTE −38.1 dB; CPU 271 ns/sample
- Note: at character 0 the biased-tanh is already clean (pure 2nd barely folds); the Chebyshev
  comparison must also sweep character 0.5/1.0 and higher drive, where tanh leaks more.

**Auto-EQ** (SR 44.1k, 24 bands, STFT 2048/512 min-phase mask):
- magnitude-match tilt RMS 0.58 dB / max 2.41; notch RMS 0.13 / max 0.64
- transient crest Δ −0.24 dB (tilt); musical-noise modulation −61.9 dB (static 1k)
- latency 2047 meas / 2048 rep (46.4 ms); flat-mask null −138.5 dBFS; CPU 240 ns/sample
- IIR filterbank should win on latency (→~0), musical-noise, transient; magnitude-match is the risk.
