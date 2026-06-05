# The Axon MelLimiter — how it works

A technical walkthrough of `native/clap/src/mel_limiter.{hpp,cpp}`. The limiter
is a **26-band Mel-scale, STFT-domain adaptive loudness maximizer** followed by
a **true-peak brickwall**. Architecturally it mirrors Newfangled Audio *Elevate*:
limit each perceptual band only as much as it needs, push the whole signal up
with drive, and cap the final peak.

```
            drive                  STFT             per-hop                 per-bin           IFFT + WOLA
 in ──┬─► ×drive ─► analysis ring ─► FFT ─► 26 Mel band ─► constrained gain ─► gain ─► apply ─► overlap-add ─► +1 smp ─► brickwall ─┬─► × wet
      │                                     levels         solve (α blend)    map           synthesis        align    (lookahead)   │
      └─► dry delay ring ───────────────────────────────────────────────────────────────────────────────────────────────────────┴─► × (1-wet) ─► out
```

Constants (`mel_limiter.hpp`): `kFFTSize=1024`, `kHopSize=256` (75 % overlap),
`kNumBands=26`, `kBrickLA=64`, `kLatency = 1024+64 = 1088` samples (≈ 24.7 ms
@ 44.1 kHz). FFT via Apple Accelerate vDSP (`zrip`). Hann window throughout.

---

## 1. Input, drive, and the dry path

Every input sample is written to two rings (`process()`):

- `in_ring[]` — the **analysis** ring, written as `sample × drive_lin`. Drive is
  applied *here only*, so pushing Drive up moves the signal into the limiter
  without touching the dry path.
- `dry_ring[]` — the **dry** delay line, written raw, read back delayed by
  exactly `kLatency` so the wet/dry blend at the end is phase-aligned.

A hop fires every `kHopSize = 256` samples.

## 2. STFT analysis (per hop, per channel)

The newest 1024 samples are linearised out of the ring, multiplied by a Hann
window, packed (`vDSP_ctoz`) and forward-transformed (`vDSP_fft_zrip`). 75 %
overlap (hop 256 / size 1024) means 4 analysis frames cover every output sample
— the basis for clean reconstruction (§7).

## 3. The Mel filterbank (`build_mel_`)

26 triangular bands are laid out on the **HTK Mel scale** from 20 Hz to 20 kHz
(perceptually even spacing — narrow in the bass, wide in the treble). Two arrays
are precomputed:

- `band_to_bin_[b·n_freq + k]` — triangular weight of FFT bin `k` in band `b`.
- `bin_norm_[k] = Σ_b band_to_bin_[b,k]` — total band weight on each bin, used
  to map band gains back to bins without changing flat-signal level (§6).

Band centre frequencies are stashed (`band_center_hz_`) for the UI visualization.

## 4. Per-band level — and the normalization that makes it meaningful

For each band the **energy** is the filterbank-weighted spectral power, taken as
the **max across channels** (linked stereo — a hot left channel limits the right
identically, preserving the image):

```
e_b = Σ_k  w_b[k] · |X[k]|²
band_level[b] = sqrt(e_b) · level_scale_
```

`level_scale_` is the subtle, critical part. Raw vDSP FFT magnitudes scale with
N, so without normalization a band's "level" is ~1000× its true amplitude and
the solver thinks every signal is wildly over the ceiling. The fix is derived
analytically: for a sine of amplitude A,

```
Σ_b e_b  =  A² · N · Σ window²        ⇒   level_scale_ = 1 / sqrt(N · Σ window²)
```

With this, a full-scale sine reads `total ≈ 1.0`, i.e. band levels are in the
same **linear-amplitude units as the ceiling**. (Σ Hann² over 1024 = 384, so
`level_scale_ = 1/sqrt(1024·384) ≈ 1/627`.) This single constant is what makes
every downstream comparison to `ceiling_lin` correct.

## 5. The constrained gain solve — the heart of it (`solve_gains_`)

Let `C = ceiling_lin` and `total = sqrt(Σ band_level²)` (the broadband level).

**If `total ≤ C`** the frame is already under the ceiling → all band gains = 1,
early-out, no limiting. (This is why, with low Drive, the adaptive controls do
nothing — the solver never engages and the brickwall does all the work.)

**Otherwise** it computes two candidate gain vectors and blends them:

### (a) Uniform
```
g_uni = C / total
```
Scale **every** band by the same ratio. Pulls `total` exactly to `C` while
preserving the spectral balance — transparent timbre, but a single loud band
drags the whole mix down (least loudness).

### (b) Reverse water-filling
Find the gains that pull total energy to `C²` while **only reducing the bands
that are too loud** and leaving quiet bands untouched. Formally: find a threshold
`λ` such that

```
Σ_n  min(1, λ / L[n])²  ·  L[n]²  =  C²
```

Bands with `L[n] ≤ λ` keep gain 1; bands with `L[n] > λ` are pulled **down to λ**
(gain `λ/L[n]`). It's the mirror of classic water-filling: instead of pouring
power into the best channels, we *skim* power off the loudest bands until the
energy budget `C²` is met.

The solve is O(N log N): sort band levels ascending, then scan the cutoff `k`.
With bands `0..k` staying at unity (their energy `accum`), the remaining
`N−k−1` bands all sit at `λ`, so

```
accum + (N−k−1)·λ²  =  C²   ⇒   λ = sqrt((C² − accum) / (N−k−1))
```

accepted at the first `k` where `sorted_L[k] ≤ λ ≤ sorted_L[k+1]` (the partition
is self-consistent: band `k` really is below threshold, band `k+1` above).

### (c) Blend
```
g[n] = (1 − α)·g_uni  +  α·g_wf      α = adaptive_gain
```
α = 0 → pure uniform (broadband, transparent). α = 1 → pure water-filling
(multiband, denser/louder, can shift tone). This is the **Adaptive Gain** knob's
job in the spectral stage.

## 6. Time-smoothing and band→bin mapping

Raw per-hop targets would zipper, so each band gain is one-pole smoothed toward
its target once per hop:

```
coef = (target < prev) ? attack : release
band_gain[b] = coef·prev + (1−coef)·target
```
Attack is fixed fast (5 ms — clamp down quickly); **release = 30 + adaptive_speed·370 ms**
(30–400 ms). That's **Adaptive Speed** in the spectral stage.

The 26 smoothed band gains are mapped back to the `n_freq` FFT bins through the
same triangular weights, normalised by `bin_norm_`:

```
bin_gain[k] = ( Σ_b w_b[k]·band_gain[b] ) / bin_norm[k]
```
The `/bin_norm` guarantees that if all band gains are 1, every bin gain is
exactly 1 (a flat unity signal passes untouched).

## 7. Apply, IFFT, and weighted overlap-add (WOLA)

The spectrum is multiplied by `bin_gain` (with care for the packed DC/Nyquist
bins), inverse-FFT'd, multiplied by the Hann **synthesis** window again, scaled
by `ola_scale_ = 1/(2N)` (the exact inverse of vDSP's zrip round-trip gain of
`2N`), and overlap-added into `out_ring`. In parallel, `window²` is overlap-added
into `norm_ring`. The output sample is

```
wet = out_ring[r] / norm_ring[r]
```

This is the WOLA normalization: signal accumulates `frame·window²`, the
denominator accumulates `window²`, and at 75 % overlap `Σ window² → 1.5`
(constant), so a unity-gain signal reconstructs **exactly** (verified to 0 %
error in the unit tests). A final 1-sample (`wet_z1`) delay trims the WOLA's
natural `kFFTSize−1` group delay up to exactly `kFFTSize`.

## 8. The true-peak brickwall (`brickwall_`)

The spectral stage controls *energy*, not instantaneous *peaks*, so a final
linked lookahead limiter pins the actual sample peak to the ceiling:

- A `kBrickLA = 64`-sample lookahead line delays the audio; the gain reacts to
  the **incoming** sample (64 samples *ahead* of what's being output), so it can
  pre-duck **before** a peak arrives.
- `g_req = C / peak` when the upcoming peak exceeds `C`, else 1; smoothed with
  attack/release into `brick_gain`.
- The delayed sample is multiplied by `brick_gain`, then **hard-clipped to ±C**.
  That clamp is the absolute guarantee: regardless of ballistics, `|out| ≤ C`.

### Even vs Dynamic (the MLA toggle)
- **Even** (off): fixed tight attack (`16`-sample time constant) + fast 50 ms
  release. Consistent, clean, transparent.
- **Dynamic** (on): the adaptive knobs reshape the brickwall —
  - **Adaptive Gain → attack character**: `atk = kBrickLA·(0.15 + gain·1.05)`
    samples. Tight (≈10 smp, fully pre-ducks inside the lookahead → clean) →
    loose (≈77 smp, *slower than the lookahead* so transients partially leak and
    the hard clip catches them → punch + a little clipper grit).
  - **Adaptive Speed → release**: 50 → 400 ms (slow = breathing/pumping).

  Attack is always bounded by the lookahead and the clip always fires last, so
  "loose" trades transient clipping for punch without ever breaking the ceiling.

## 9. Two independent ceilings

1. **The brickwall clip above** — inside the limiter, at the limiter's own
   `ceiling_lin` (the MLC control). This is the maximizer's output cap.
2. **`TruePeakCeiling`** — a *separate*, always-on, plugin-level stage that runs
   dead last in the chain (`axon_plugin.cpp`), 4×-oversampled true-peak, default
   −1 dBTP from the bundle meta. It's the whole-signal-path safety net (catches
   overshoot from *any* module, e.g. the saturator) and is independent of the
   limiter.

## 10. Latency

`kLatency = kFFTSize + kBrickLA = 1088` samples. The dry ring is delayed by this,
the wet path's STFT contributes `kFFTSize` (after the 1-sample trim) and the
brickwall adds `kBrickLA`, so wet and dry stay sample-aligned. The plugin reports
this to the host for delay compensation.

## Control summary

| Control | Spectral stage (always) | Brickwall — Even | Brickwall — Dynamic |
|---|---|---|---|
| **Drive** (MLD) | input gain into analysis ring (loudness) | — | — |
| **Ceiling** (MLC) | target `C` for the solve | hard cap `±C` | hard cap `±C` |
| **Adaptive Gain** (MLG) | uniform ↔ water-fill blend `α` | — | **attack character** |
| **Adaptive Speed** (MLS) | release 30–400 ms | — | **release 50–400 ms** |
| **Dynamic** (MLA) | — | fixed tight/fast | routes MLG/MLS into the brickwall |
| **Limiter** (MLI) | wet/dry mix | | |

## Why this design

- **Mel bands** put the resolution where hearing is, so loudness can be pushed
  without the bass dragging the highs down (or vice versa).
- **Water-filling** is the energy-optimal way to meet a ceiling: reduce only what
  must be reduced. The `α` blend lets you dial from transparent (uniform) to
  aggressive (multiband).
- **STFT gain** gives smooth, frequency-selective reduction; WOLA reconstructs
  transparently when nothing is being limited.
- **Lookahead brickwall** turns the energy-domain limiter into a true peak limiter,
  and exposing its *attack/release* (Dynamic mode) is what gives the limiter its
  character — clamped and even, or breathing and punchy.

## Where it's verified

`tests/test_mel_limiter.cpp` (12 units, asserts forced on via `-UNDEBUG`):
silence, exact dry-bypass delay, ceiling reduction, NaN/extremes, reset, stereo
link, **0 % WOLA reconstruction**, brickwall peak cap, drive loudness, ceiling
audibility, adaptive-brickwall release, adaptive-brickwall attack. The LUFS in/out
meter is cross-checked against the reference `LufsLeveler` in `tests/test_meter.cpp`.
