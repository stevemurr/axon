# Sonic-improvement harness & workflow — Auto-EQ

Goal: make "better" **falsifiable**. A candidate algorithm ships only if it beats
the current one on named, reproducible metrics *at matched musical effect*, and
the win is large enough to be audible. Otherwise it is rejected with the numbers.

Candidate under test:
- **Auto-EQ** → minimum-phase IIR peaking/shelf filterbank replacing the STFT
  magnitude mask (same neural band targets — controller unchanged, renderer swapped).

(The harness previously also covered the retired Exciter/Harmonics stage; that
stage and its `harness_exciter.cpp` / `harness_harmonics.cpp` were deleted in
2026-07 when the stage was removed from the plugin.)

## The one principle: match the effect, then compare the artifacts
A processor that does *less* trivially distorts less. So every A/B normalizes the
musical effect first, then measures the artifacts:
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
bench/sonic/harness_auto_eq.cpp baseline vs candidate -> table   (links Accelerate for SpectralMaskEq)
```
Each DUT is wrapped as `std::function<void(const float* in, float* out, int n)>`
so baseline and candidate run through the identical measurement path and print a
single comparison table. Deterministic (fixed seeds); prints a human table plus
`key=value` lines for scripting.

---

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
- [x] harness_auto_eq.cpp + baseline
- [x] min-phase IIR filterbank auto-EQ candidate + A/B

## Candidate results (2026-06-07)
**Auto-EQ — min-phase IIR filterbank** (`IirFilterbankEq`, 24 bells + edge
shelves, A⁻¹ solve so the summed response hits the band targets):
- Magnitude-match: tilt 0.43 dB RMS (beats STFT's 0.58); notch 0.21 RMS (under
  the 0.5 bar; STFT's per-bin mask is tighter on a narrow notch: 0.13 / max 0.64
  vs 0.21 / max 1.58).
- CPU ~4.4× cheaper (55 vs 240 ns/sample); latency 0 vs 2048 (46 ms); flat-mask
  reconstruction exact (−240 vs −138 dBFS).

### The clarity/transient edge was REFUTED by the strengthened metrics
The original rationale for the IIR was "less STFT smearing / less musical noise."
Both were tested directly and do NOT hold:
- **Transient (impulse-response spread under an active curve):** STFT is actually
  MORE compact — tilt RMS-duration 0.065 ms / −40 dB tail 0.48 ms vs the IIR's
  0.096 ms / 1.07 ms. Both have no pre-echo (−110 / −240 dB). The min-phase STFT
  concentrates energy compactly (the reason min-phase was chosen); the IIR's 24
  resonant bells ring slightly more.
- **Musical noise (steady tone, moving mask at 1/3/8 Hz):** a wash — STFT and IIR
  give identical spurious sidebands (≈ −45.7 dB, Δ ≤ 0.2 dB at every rate). Both
  smooth at block rate, and a smooth-mask EQ (unlike a spectral gate) has no
  musical-noise mechanism to begin with.

**Corrected verdict:** the IIR filterbank is a **latency + CPU** win (and exact
flat reconstruction), NOT a sonic/clarity improvement. The existing min-phase
STFT Auto-EQ is already clean on transients and automation. So as a *sonic*
upgrade it is a no-op; adopt it only if the 46 ms latency or the CPU matter.
This is exactly the false-"better" the match-the-effect harness exists to catch.

## Baselines (captured 2026-06-07; Release -O3, Apple Silicon)
**Auto-EQ** (SR 44.1k, 24 bands, STFT 2048/512 min-phase mask):
- magnitude-match tilt RMS 0.58 dB / max 2.41; notch RMS 0.13 / max 0.64
- transient crest Δ −0.24 dB (tilt); musical-noise modulation −61.9 dB (static 1k)
- latency 2047 meas / 2048 rep (46.4 ms); flat-mask null −138.5 dBFS; CPU 240 ns/sample
- IIR filterbank should win on latency (→~0), musical-noise, transient; magnitude-match is the risk.
