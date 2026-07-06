# The Mel Limiter: a 26-band water-filling loudness maximizer, and how to prove a 3x rewrite changed nothing

> Source of truth: `native/clap/src/mel_limiter.{hpp,cpp}` (all `file:line`
> anchors verified against `origin/main` as of commit `28f27bd`), plus
> `native/clap/src/stft_common.hpp`, `native/clap/src/mel_scale.hpp`, the
> plugin integration in `native/clap/src/axon_plugin.cpp`, the 19-unit suite in
> `native/clap/tests/test_mel_limiter.cpp`, and the micro-benchmark
> `native/clap/bench/bench_mel_limiter.cpp`.
>
> This document supersedes `docs/mel_limiter.md`,
> `native/clap/docs/limiter_algorithm.md`, and
> `native/clap/docs/mel_limiter_perf_findings.md`.

Every mastering chain ends with the same request: make it loud, keep it under
the ceiling, and don't let the kick drum duck the vocals. A broadband limiter
can't do all three — it sees the whole signal as one number, so a loud bass hit
pulls *everything* down and you hear the mix pump. Axon's answer is the
MelLimiter, a stage that mirrors Newfangled Audio's *Elevate*: split the
spectrum into **26 perceptually-spaced mel bands**, and when the total energy
exceeds the ceiling, **skim energy off only the bands that are too loud** — a
reverse water-filling solve — then catch the residual sample peaks with a
lookahead brickwall.

The module also carries two of the best war stories in the codebase:

1. **It once crushed all audio to near silence** — not because the algorithm
   was wrong, but because a single missing normalization constant made the
   solver believe every signal was ~627x over the ceiling.
2. **Its hot loop was rewritten 3x faster in July 2026 and shipped** even
   though the module is so numerically ill-conditioned that *an unchanged
   binary cannot pass the project's own null test*. Proving that rewrite
   "changed nothing" required first measuring what "nothing" looks like.

Both stories are told in full below, because both changed how this repo
validates DSP work.

---

## 1. The architecture in one screen

```
            drive                  STFT             per-hop                 per-bin           IFFT + WOLA
 in ──┬─► ×drive ─► analysis ring ─► FFT ─► 26 mel band ─► constrained gain ─► gain ─► apply ─► overlap-add ─► +1 smp ─► brickwall ─┬─► × wet
      │                                     levels         solve (α blend)    map           synthesis        align    (lookahead)  │
      └─► dry delay ring ───────────────────────────────────────────────────────────────────────────────────────────────────────────┴─► × (1−wet) ─► out
```

Two stages, two jobs. The **spectral stage** controls *energy* — how loud each
frequency region is on average, per 1024-sample analysis frame. The
**brickwall** controls *peaks* — no individual sample may exceed the ceiling,
guaranteed by a hard clamp. Energy shaping is where the loudness and the
character live; the brickwall is the safety net that makes the ceiling a real
ceiling.

Constants (`mel_limiter.hpp:23-27`):

| Constant | Value | Meaning |
|---|---|---|
| `kFFTSize` | 1024 | FFT/window length (the STFT frame) |
| `kHopSize` | 256 | samples between frames → 75% overlap, 4 frames per sample |
| `kNumBands` | 26 | mel bands, 20 Hz – 20 kHz |
| `kBrickLA` | 256 | brickwall lookahead (~5.8 ms @ 44.1 kHz) |
| `kLatency` | 1280 | `kFFTSize + kBrickLA`, reported to the host |

Host-facing controls, with bundle defaults from
`weights/axon_bundle/axon_meta.json` (resolved in `axon_plugin.cpp:1414-1417`,
converted at `:1453-1458`):

| Control (ID) | Internal param | Spectral stage (always) | Brickwall — Even | Brickwall — Dynamic | Range / default |
|---|---|---|---|---|---|
| **Drive** (MLD) | `drive_lin = 10^(MLD/20)` | input gain into the analysis ring only — pushes the signal into the ceiling | — | — | 0–24 dB, default **2 dB** |
| **Ceiling** (MLC) | `ceiling_lin = 10^(MLC/20)` | target `C` for the gain solve | hard cap `±C` | hard cap `±C` | −12–0 dBFS, default **0 dBFS** |
| **Adaptive Gain** (MLG) | `adaptive_gain` (α) | uniform ↔ water-fill blend | — | **attack character** (tight↔loose) | 0–1, default 0.5 |
| **Adaptive Speed** (MLS) | `adaptive_speed` | release 30→400 ms | — | **release** 50→400 ms | 0–1, default 0.5 |
| **Dynamic** (MLA) | `adaptive_brickwall` | — | fixed tight/fast | routes MLG/MLS into the brickwall | switch, default **ON** |
| **Limiter** (MLI) | `wet_mix` | wet/dry mix of the whole module; at 0 the stage is skipped and its latency is dropped | | | 0–1, default 1 |

Note `adaptive_gain` and `adaptive_speed` are deliberately dual-purpose: they
always shape the spectral stage, and in Dynamic mode they *additionally*
reshape the brickwall ballistics. (This dual identity is a documented design
smell — see §8.)

---

## 2. The founding bug: one missing constant crushed all audio

The per-band level is the filterbank-weighted spectral power, linked across
channels (max energy per band), square-rooted into amplitude units
(`mel_limiter.cpp:166-187`):

```
e_b           = Σ_k  w_b[k] · |X[k]|²          (max over channels)
band_level[b] = sqrt(e_b) · level_scale_
```

`level_scale_` is the single most load-bearing constant in the module. Raw
vDSP `zrip` FFT magnitudes are unnormalized — they grow with the frame size N
and the analysis window. The original code compared raw FFT-domain energy
directly against the linear-amplitude `ceiling_lin`. A quiet 0.01-amplitude
sine read as `total ≈ 6.27` against a ceiling of 0.891, so the solver applied
~0.14x gain to *everything*, at any input level. The plugin "didn't pass
audio."

The fix was derived analytically, not tuned: for a Hann-windowed sine of
amplitude A, the summed FFT-domain band energy is `A² · N · Σ window²`, so the
conversion back to linear amplitude is

```
level_scale_ = 1 / sqrt(N · Σ window²)
```

(`mel_limiter.cpp:44-48`, rationale restated at `mel_limiter.hpp:121-125`).
With N = 1024 and `Σ Hann² = 3N/8 = 384`, `level_scale_ = 1/sqrt(393216) ≈
1/627` — which is exactly the ~627x inflation the solver was seeing. After the
fix a full-scale sine reads `total ≈ 1.0`: band levels are in the **same
linear-amplitude units as the ceiling**, which is what makes every comparison
downstream meaningful. The derivation matched measurement exactly.

If the window or FFT size ever changes, the constant recomputes itself in
`init()` — but any *new* band-level math must preserve this scaling, or the
solver silently goes insane again. That is why regression pinning lives in the
unit tests (`test_ceiling_reduces_loud_signal`, `test_drive_increases_loudness`
— `test_mel_limiter.cpp:153-179,353-385`).

The same debugging session found the second founding bug: the WOLA wet path
has a natural group delay of `kFFTSize − 1 = 1023` samples, but the dry ring
and the reported latency assume 1024. A **single-sample `wet_z1` delay** on the
wet drain (`mel_limiter.hpp:79-82`, applied at `mel_limiter.cpp:425-429`) trims
the group delay to exactly `kFFTSize`. Without it the wet/dry blend combs. If
wet+dry ever sounds phasey, suspect this alignment first;
`test_stft_reconstruction` (`test_mel_limiter.cpp:283-321`) pins wet-vs-dry
agreement to <3% relative error, and `test_dry_bypass` pins the dry path to an
*exact* `kLatency` delay.

The meta-lesson recorded from that session: the fix came from running the
existing unit tests first, then writing tiny isolated probes (FFT round-trip
scale, per-frame synthesis value, solver total-vs-ceiling) to bisect which
*unit* was wrong — not from re-reading the whole file.

---

## 3. Signal flow, top to bottom

### 3.1 Input, drive, and the dry path

Every input sample is written to two rings (`mel_limiter.cpp:327-335`):

- `in_ring` — the **analysis** ring, written as `sample × drive_lin`. Drive is
  applied *here only*: turning Drive up pushes the wet path into the ceiling
  without touching the dry path, so the module behaves as a loudness maximizer
  (drive in, capped out) rather than a plain attenuator.
- `dry_ring` — the raw input, read back delayed by exactly `kLatency` for the
  wet/dry blend (`mel_limiter.cpp:440-445`).

A hop fires every `kHopSize = 256` samples, triggered off channel 0 (both
channels accumulate at the same rate, `mel_limiter.cpp:337-339`).

### 3.2 STFT analysis

Per hop and channel, the newest 1024 samples are linearized out of the ring and
Hann-windowed via two contiguous `vDSP_vmul` segments, then packed and
forward-FFT'd (`vDSP_ctoz` + `vDSP_fft_zrip`). Since mid-2026 this scaffolding
is shared across all of Axon's spectral units through `stft_common.hpp` —
`make_hann`, `window_ring_oldest_first`, `forward_zrip`, and the
`OlaAccumulator` are verbatim code motion from the original per-module copies,
so renders stayed byte-identical (`mel_limiter.cpp:341-349`,
`stft_common.hpp:20-45`). 75% overlap means 4 analysis frames cover every
output sample — the redundancy that makes reconstruction clean (§3.7).

One vDSP packing detail recurs everywhere: `zrip` stores the DC bin's real
part in `realp[0]` and the **Nyquist bin's real part in `imagp[0]`**. Both the
solver (`mel_limiter.cpp:174-175`) and the gain apply
(`mel_limiter.cpp:392-394`) special-case element 0 for this reason. Porting to
another FFT backend means re-checking every one of those sites.

### 3.3 The mel filterbank (`build_mel_`, `mel_limiter.cpp:98-155`)

26 triangular bands are laid out on the **HTK mel scale** from 20 Hz to
20 kHz, using the shared conversion templates in `mel_scale.hpp:18-22`:

```
mel(f) = 2595 · log10(1 + f/700)      f(mel) = 700 · (10^(mel/2595) − 1)
```

28 equally-spaced mel points are converted back to Hz, then to (fractional)
FFT-bin positions (`mel_limiter.cpp:103-109`). Band `b` is a triangle peaking
at point `b+1`, ramping up from point `b` and down to point `b+2`:

```
band_to_bin[b][k] = max(0, min((k − lo)/ls, (hi − k)/rs))
```

Because the points are even *in mel*, triangles are narrow in the bass and wide
in the treble — each band carries roughly equal perceptual weight. Band-centre
frequencies are stashed in `band_center_hz_` for the UI x-axis
(`mel_limiter.cpp:117`), and `bin_norm_[k] = Σ_b w_b[k]` records the total band
weight on each bin (`mel_limiter.cpp:127-130`) — the key to transparent
reconstruction in §3.6.

`build_mel_` then precomputes the **sparse acceleration structures** added by
the 2026-07 rewrite (`mel_limiter.cpp:132-154`, members at
`mel_limiter.hpp:99-111`):

- `band_start_[b]` / `band_len_[b]` — each triangle touches only a contiguous
  bin span; only 870 of the 26x513 = 13,338 dense-matrix entries are nonzero
  (15.3x sparse).
- `band_to_bin_nrm_` — the triangle weights *pre-divided* by `bin_norm_`,
  folding the per-bin normalization divide into the stored weights.
- `bin_gain_tmpl_` — 1.0 at bins no triangle reaches (they must pass through
  with unity gain, matching the old dense path's `bin_norm_ ≤ 1e-6` branch),
  0 elsewhere.

### 3.4 The constrained gain solve (`solve_gains_`, `mel_limiter.cpp:161-230`)

This is the heart of the module. Per hop:

**Band energies, vectorized.** The power spectrum is computed **once per
channel** with `vDSP_zvmags` (with DC/Nyquist patched per the packing above),
then each band's energy is **one sparse `vDSP_dotpr`** over its nonzero span
(`mel_limiter.cpp:171-183`). Linked stereo takes the max energy per band across
channels, so both channels later receive identical gains and the image never
wanders. Band levels also feed the display tap `disp_level_`
(`mel_limiter.cpp:184-187`).

**Early-out.** With `C = ceiling_lin` and `total = sqrt(Σ band_level²)`, if
`total ≤ C` the frame is already under the ceiling: all band gains stay 1
(`mel_limiter.cpp:195-198`). Consequence worth internalizing: **at low Drive
the spectral solver never engages** — `total` is a quasi-RMS quantity that for
normal material sits 12–15 dB below the sample peak, so the brickwall does all
the visible work and the adaptive knobs appear dead. That is by design, and it
is also the root of the issue-#17 knob-feel findings (§8).

**Candidate (a): uniform.**

```
g_uni = C / total                      (mel_limiter.cpp:201)
```

Scale every band by the same ratio. Pulls `total` exactly to `C`, perfectly
preserves spectral balance — transparent, but a single loud band drags the
whole mix down. Least loudness.

**Candidate (b): reverse water-filling.** Reduce *only* the bands that are too
loud, leaving quiet bands untouched: find a water level `λ` such that

```
Σ_n  min(1, λ/L[n])² · L[n]²  =  C²
```

Bands with `L[n] ≤ λ` keep gain 1; bands with `L[n] > λ` are pulled down *to*
`λ` (gain `λ/L[n]`). It is the mirror image of classic information-theoretic
water-filling: instead of pouring power into the best channels, we skim power
off the loudest bands until the energy budget `C²` is met — the energy-optimal
way to satisfy the ceiling with minimum total gain reduction.

The solve is a sort + linear scan (`mel_limiter.cpp:207-221`). Sort band
levels ascending; hypothesize a cutoff `k` where bands `0..k` stay at unity
(accumulated energy `accum`) and the remaining `N−k−1` bands all sit at `λ`:

```
accum + (N−k−1)·λ²  =  C²    ⇒    λ = sqrt((C² − accum) / (N−k−1))
```

Accept the first `k` where `sorted_L[k] ≤ λ ≤ sorted_L[k+1]` — that inequality
pair is exactly the self-consistency proof that the partition is right (band
`k` genuinely below the water level, band `k+1` genuinely above). Two guarded
exits: if `accum` alone already exceeds `C²`, `λ = 0` (`mel_limiter.cpp:215`);
if no cutoff is accepted, `λ` falls back to `g_uni`
(`mel_limiter.cpp:210`). Note for later: this sort-plus-branch structure is
*discontinuous* — an infinitesimal input change can flip the selected cutoff —
and that discontinuity is a main character in the null-test story (§7).

**Blend.**

```
g[n] = clamp((1 − α)·g_uni + α·g_wf, 0, 1),    α = adaptive_gain
                                               (mel_limiter.cpp:223-229)
```

α = 0 is pure uniform (broadband, transparent); α = 1 is pure water-filling
(multiband, denser and louder, can shift tone). That is the **Adaptive Gain**
knob's spectral-stage job.

### 3.5 Time smoothing (`mel_limiter.cpp:297-302, 357-363`)

Raw per-hop targets would zipper audibly, so each band gain is one-pole
smoothed once per hop, with asymmetric ballistics:

```
hop_ms = 1000·kHopSize/sr ≈ 5.8 ms @ 44.1 kHz
atk_c  = exp(−hop_ms / 5 ms)                       fixed fast attack
rel_c  = exp(−hop_ms / (30 + adaptive_speed·370) ms)   release 30–400 ms

band_gain[b] = c·prev + (1−c)·target,   c = (target < prev) ? atk_c : rel_c
```

Attack clamps down fast when a band gets loud; **release is the Adaptive Speed
control** (snappy to breathing).

### 3.6 Band → bin mapping (`mel_limiter.cpp:365-377`)

The 26 smoothed band gains are spread back onto the 513 bins. Mathematically:

```
bin_gain[k] = ( Σ_b w_b[k] · band_gain[b] ) / bin_norm[k]
```

The `/bin_norm[k]` is what guarantees that all-unity band gains produce
*exactly* unity bin gains — an idle limiter adds zero coloration. In code this
is now the sparse prenormalized form: seed `bin_gain_arr_` from
`bin_gain_tmpl_` (uncovered bins get 1), then one `vDSP_vsma`
(scalar-multiply-accumulate) per band over its nonzero span, using the
pre-divided weights. Same math, ~6.5% of the memory traffic.

`bin_gain_arr_` and the power-spectrum scratch `pwr_` are **pre-allocated in
`init()`** (`mel_limiter.cpp:33-36`, `mel_limiter.hpp:107-111`). Older docs
called out "one per-block heap allocation of `bin_gain_arr` inside `process()`"
as the module's RT-audit blemish — that is no longer true. The hoist is pinned
by two dedicated regression tests: `test_bin_gain_scratch_reuse_block_invariance`
(chunked processing at awkward block sizes must match a one-shot run **exactly,
max error 0.0**) and `test_bin_gain_scratch_init_state` (first-call-after-init
correctness plus two fresh instances agreeing bit-for-bit)
(`test_mel_limiter.cpp:590-684`).

### 3.7 Apply, IFFT, and weighted overlap-add (`mel_limiter.cpp:379-414`)

The spectrum is multiplied by `bin_gain_arr` in place (DC/Nyquist
special-cased, the interior bins via two `vDSP_vmul`s), inverse-FFT'd,
multiplied by the Hann **synthesis** window, and scaled by
`ola_scale_ = 1/(2N)` — the exact inverse of vDSP's `zrip` forward+inverse
round-trip gain of `2N` (verified: exactly 2048, `mel_limiter.cpp:31`).

The windowed frame goes into the shared `OlaAccumulator`
(`stft_common.hpp:47-125`): the audio accumulates into `out_ring` while
`window²` accumulates into `norm_ring`, and the per-sample drain returns

```
wet = out_ring[r] / norm_ring[r]
```

This is torch.istft-style WOLA normalization: at 75% overlap the four
overlapping Hann² frames sum to exactly 1.5, and a unity-gain signal
reconstructs with 0% error in steady state (asserted <3% including edge
effects by `test_stft_reconstruction`). One shared-scaffolding subtlety:
`add_frame` takes a `clamp_avail` policy flag, and MelLimiter keeps its
historical *unclamped* `avail` accounting (`mel_limiter.cpp:412-413`) —
unifying it with SpectralMaskEq's clamped policy would have been a behavior
change, and the whole point of `stft_common.hpp` was byte-identical code
motion.

Then the `wet_z1` one-sample alignment from §2, and on to the brickwall.

### 3.8 The lookahead brickwall (`brickwall_`, `mel_limiter.cpp:250-286`)

The spectral stage controls energy; a signal with correct frame energy can
still spike above the ceiling for a few samples. The brickwall pins the actual
sample peak, linked stereo:

- A `kBrickLA = 256`-sample lookahead ring delays the wet audio.
- The gain targets **the loudest sample anywhere in the lookahead window**,
  found with a **monotonic-deque sliding-window maximum**
  (`mel_limiter.cpp:256-271`, state at `mel_limiter.hpp:133-139`): new
  magnitudes evict smaller entries from the back (they can never be the max
  again), stale entries expire off the front, and the front is always the
  window max — O(1) amortized per sample, fixed `kDqCap = 257` storage. The
  window (vs. a naive single-sample tap) is the entire trick: the gain sees the
  worst upcoming peak *the moment it enters the window* and has the full 256
  samples to ramp down before that peak reaches the output.
- `g_req = C / wmax` when the window max exceeds `C`, else 1; one-pole smoothed
  into `brick_gain_` with attack on the way down, release on the way up
  (`mel_limiter.cpp:271-277`).
- The delayed sample is multiplied by `brick_gain_` and then **hard-clamped to
  ±C** (`mel_limiter.cpp:279-284`). The clamp is the unconditional guarantee:
  whatever the ballistics do, `|out| ≤ C`. Always.

Because the gain is already down when a peak arrives, the clamp barely fires in
the clean modes — and that is measurable. On a 60 Hz tone driven 6 dB over the
ceiling, the limiter measures **~0.1% THD versus ~22.7% for a pure clipper**;
`test_lookahead_low_distortion` asserts the limiter under 0.4x the clipper's
residual (`test_mel_limiter.cpp:543-572`). Bass is exactly where this matters:
a 60 Hz half-cycle is ~370 samples, so a no-lookahead limiter has no choice but
to shave the wave crest (odd-harmonic distortion), while the windowed detector
ducks the whole cycle cleanly.

**Even vs Dynamic** (per-block ballistics, `mel_limiter.cpp:304-320`):

- **Even** (`adaptive_brickwall = false`): fixed tight attack, time constant
  `kBrickLA·0.25 = 64` samples (converges well inside the window), fixed 50 ms
  release. Consistent and clean. (An older doc said "16-sample time constant" —
  that was never what shipped; the code is `kBrickLA * 0.25f`.)
- **Dynamic** (`adaptive_brickwall = true`, the bundle default): the adaptive
  knobs reshape the brickwall —
  - **Adaptive Gain → attack character**:
    `atk_samps = kBrickLA·(0.15 + adaptive_gain·1.05)`, i.e. ~38 samples
    (tight: fully pre-ducks inside the lookahead, transparent) up to ~307
    samples (**deliberately slower than the 256-sample window**, so transients
    partially leak past the smooth gain and hit the hard clamp — punch plus a
    little clipper grit).
  - **Adaptive Speed → release**: 50→400 ms (slow = audible breathing).

  Attack is bounded by the lookahead and the clamp always fires last, so
  "loose" trades transient clipping for punch *without ever breaking the
  ceiling* — `test_adaptive_brickwall_attack`/`_release` isolate and assert
  both behaviors with a burst-on-quiet-tone signal engineered so the spectral
  solver stays inert (window energy below C, peak above it).

One naming caveat, promoted from the perf findings: despite the "true-peak"
comment in the source, this detector is a **sample-peak** limiter — no
oversampling, so inter-sample peaks can exceed `C` between samples. That is
correct *by architecture*: the always-on, 4x-oversampled `TruePeakCeiling`
stage runs dead last in the plugin chain and owns inter-sample peaks for the
whole signal path. Two independent ceilings, two jobs: MLC caps this module's
sample peaks; TruePeakCeiling is the chain-wide dBTP net.

---

## 4. Plugin integration (`axon_plugin.cpp`)

- **Stage.** `StageID::MelLimiter = 5` (`axon_plugin.cpp:111`), instance at
  `:690`, reorderable like every stage. The dispatch case (`:2038-2050`) fills
  `Params` from the amount snapshot and calls `process()` on the working
  block; `ml_wet ≤ 0` skips the stage entirely.
- **Controls.** `resolve_amount_` reads MLI/MLC/MLD/MLG/MLS/MLA
  (`:1414-1417`) and converts dB → linear (`:1453-1458`). Per the meta↔C++
  contract, the control read-set here must equal the `axon_meta.json` controls
  list — the contract test enforces it. The MLx controls live *only* in
  `axon_meta.json`, `axon_plugin.cpp`, and `ui/index.html`; no Python defines
  them, and CLAP param IDs are stable string hashes so adding controls never
  scrambles automation.
- **Lifecycle.** `init(sample_rate)` at activation (`:1095`); `reset()` on
  plugin reset (`:1285`). Both are allocation-free after `init`.
- **Latency.** `compute_latency_` adds `MelLimiter::kLatency` **only when
  `MLI > 0`** (`:975-976`), so toggling the limiter changes reported plugin
  latency and the host re-runs PDC.
- **Display taps.** After each processed block the audio thread snapshots
  `copy_display()` (band levels, smoothed gains, static centres) plus the
  brickwall gain under a **`try_lock`** — never blocking audio (`:2411-2420`).
  The GUI timer converts to dB and pushes
  `axonLimiter({active, brick, ceiling, f[], lvl[], gr[]})` at ~21 fps
  (`:2650-2689`); the UI draws 26 bars (level + red gain-reduction cap).
  `copy_display`'s sizing contract (`num_bands()` == 26, HTK-mel centres) is
  verified against an independent double-precision oracle with a derived — not
  tuned — error bound (`test_num_bands_and_mel_centers`,
  `test_mel_limiter.cpp:706-746`).

---

## 5. Latency, cost, and real-time safety

- **Latency is exactly `kLatency = 1280` samples** (~29 ms @ 44.1 kHz, ~26.7 ms
  @ 48 kHz): `kFFTSize` from the STFT path (after the `wet_z1` trim) plus
  `kBrickLA` from the brickwall. The dry ring keys off `kLatency`
  (`mel_limiter.cpp:52`), so growing `kBrickLA` re-aligns everything
  automatically. (The header's line-9 banner comment still says "Latency:
  kFFTSize samples" — stale text from before the brickwall; `kLatency` at
  `mel_limiter.hpp:27` is the truth.)
- **Cost after the 2026-07 rewrite:** ~6.0 µs per 128-sample stereo block
  in-chain (1.45% share), ~49 ns/sample standalone at 48 kHz
  (`bench_mel_limiter.cpp`; ~0.24% of a 128-block budget). The remaining cost
  is the per-hop FFTs (fwd+inv per channel per 256 samples) plus a ~29
  ns/sample brickwall/drain loop that the perf pass explicitly ranked
  leave-alone (poor ROI).
- **RT safety:** `process()` and `reset()` perform **no heap allocation** —
  every ring and scratch buffer (`bin_gain_arr_`, `pwr_` included) is sized in
  `init()`. The old per-block `std::vector` allocation is gone and
  regression-tested (§3.6).
- **Backend:** Apple Accelerate vDSP, macOS arm64 only on `origin/main` (a
  cross-platform port is in progress on `feat/xplat`). Porting notes: replace
  `zrip` and re-audit every DC/Nyquist packing site, and re-run the null
  methodology of §7 — with the caveats described there.

---

## 6. The 3x rewrite (shipped July 2026, commit `28f27bd`)

The per-stage timing pass (`perf_stage_ranking.md`) measured MelLimiter at
18.0 µs/block — 4.0% of process time, third behind the two ONNX models — and
profiling put **66% of the module in `solve_gains_`'s band-energy loop**: the
old code recomputed `re²+im²` for all 513 bins *inside every band* and walked
the dense 26x513 mel matrix even though 93% of it is zeros.

The rewrite (proposals #5 + #10 of the pass) is the sparse/vectorized solver
described in §3.3–3.6:

1. **Power spectrum once per channel** (`vDSP_zvmags`) instead of 26
   re-derivations — the redundancy was the biggest single cost.
2. **One sparse `vDSP_dotpr` per band** over its contiguous nonzero span
   (870/13,338 nonzeros — 15.3x sparse).
3. **Prenormalized sparse band→bin map**: weights pre-divided by `bin_norm_`
   at build time, accumulated per-hop with sparse `vDSP_vsma`, uncovered bins
   seeded from a template.
4. **Vectorized per-bin gain apply** (two `vDSP_vmul`s instead of a scalar
   loop).

Measured results: **18.0 → 6.0 µs/block in-chain (share 4.0% → 1.45%);
standalone 143 → 49 ns/sample** — 3x on the module. All 19 unit tests pass
unchanged.

And then the interesting part: the rewrite is *not* bit-identical — vDSP
reassociates the energy sums — so it had to be validated by a null test against
the previous build. Which is where this module gets philosophical.

---

## 7. How do you prove a rewrite changed nothing when *nothing* doesn't null?

Axon's standard bar for a pure refactor or micro-optimization is a two-binary
null test: render the same material through the old and new plugin, subtract,
and demand the residual below **−120 dBFS**. Well-conditioned stages (BassMono,
Reverb, Widener, TruePeakCeiling) null to *exactly 0.0* under this test. The
MelLimiter cannot — **even when nothing changed**.

### 7.1 Measuring the no-change envelope

The decisive experiment (from the perf pass, module 7): compile **two copies of
the identical MelLimiter source into one binary**, distinguished only by a
renamed namespace, and diff their outputs on the same input. Result:

| Region | Identical source, two instances |
|---|---|
| Warmup transient (first ~40 ms) | **1.1e-4 ≈ −79 dBFS** |
| Steady state | **2.7e-7 ≈ −131 dBFS** |

A no-op fails the −120 dBFS bar in the warmup region. The module is
deterministic run-to-run (same binary, same instance: diff exactly 0.0, pinned
by `test_brickwall_gain_accessor`'s bitwise-identical gain trace and the exact
0.0 asserts in tests 14/15/18) — but *any* change to codegen or buffer layout
shifts the output above the bar, even with `-ffp-contract=off
-fno-vectorize`.

### 7.2 Why: two ULP amplifiers in series

1. **The water-fill solver is discontinuous.** `std::sort` plus a
   branch-selected cutoff (`mel_limiter.cpp:207-221`) means a 1-ULP
   perturbation in one band level can flip a sort comparison or the
   `lam >= sorted_L[k]` acceptance test, selecting a different partition — a
   *finitely different* λ from an infinitesimally different input. Downstream
   smoothing shrinks but never erases it.
2. **vDSP FFT rounding is buffer-alignment sensitive.** The exact rounding of
   `vDSP_fft_zrip` depends on pointer alignment; recompiling moves
   allocations, which moves alignments, which moves the last bits of every
   spectrum.

Warmup is worst because the OLA `norm_ring` is still filling (near-zero
denominators amplify relative error) and the first solver decisions cascade
through the band-gain smoother's recursive state. Once state saturates, the
instances converge to ~−131 dBFS.

The companion case is the Auto-EQ's `SpectralMaskEq`, where a proven
bit-identical primitive swap (`vDSP_vdiv` vs scalar `/`) nulls to 0.0 with a
static mask but −89 dBFS with a moving one — the min-phase cepstrum chain plus
a recursive smoother is the amplifier there. These two stages are the repo's
documented ill-conditioned pair; everything else nulls exactly.

### 7.3 The candidate that was reverted (and the one that shipped)

The methodology has teeth in both directions.

**Reverted:** the first optimization candidate replaced the per-sample runtime
`% out_ring.size()` / `% dry_sz` (true integer divisions — the moduli are
`vector::size()`, not compile-time constants) with compare-and-reset wraps.
Index equivalence was proven *exhaustively* over the full range — zero
mismatches; algebraically a no-op. The null test still showed warmup
−75.6 dBFS / steady −118.7 dBFS — i.e. exactly the no-change envelope, nothing
more. But under the then-current rule ("if you can't null it, revert") and with
a measured win of ~14% of a 0.69%-share module (~0.1% of block budget), it was
reverted and written up instead. Not worth shipping unprovable. (The perf pass
later re-measured the idea at noise and ranked it permanently leave-alone.)

**Shipped:** the 3x solver rewrite changed the math's association order, so
bit-identity was never on the table. The proof offered instead:

- **Steady-state null vs the pre-change build: −129.7 dBFS** — inside the
  measured no-change instance envelope (−119..−131 dBFS).
- **Full-signal residual −69.5 dBFS, confined to the first 0.5 s of warmup** —
  the same shape and scale as the no-change warmup variance.
- **Algebraic argument** that every transformation is a reassociation or an
  exact-zero-term elimination (the sparse spans skip only exact zeros; the
  prenormalization moves a divide across a multiply; the template reproduces
  the uncovered-bin branch).
- **All 19 unit tests**, including the exact-0.0 block-invariance and
  instance-identity tests *within* the new binary.
- **The hard-clip ceiling guarantee is untouched** — `std::clamp(o, -ceiling,
  ceiling)` makes `|out| ≤ ceiling` unconditional regardless of FP noise.

The owner explicitly accepted **steady-state-only null validation** for this
stage, and that policy is now the documented norm: for MelLimiter (and
SpectralMaskEq), validate micro-opts by steady-state null with an inaudible
tolerance (~−110 dBFS or better), skip the warmup, and lean on algebraic or
index-equivalence proofs — do not demand full-signal bit-identity, because the
stage cannot meet it *even unchanged*. The variance is inaudible (below the
16-bit noise floor, behind a limiter), but the epistemology matters: **before
you can prove a change did nothing, you must measure what nothing looks
like.**

---

## 8. Knob feel at the defaults (issue #17, measured 2026-07-05)

A user report — "the attack/release knobs do nothing" — triggered a full
plumbing audit. Verdict: the plumbing is intact end-to-end (MLG/MLS/MLA all in
the meta contract and the `resolve_amount_` read-set; MLA defaults ON). The
root cause is **gain staging at the defaults**: Drive 2 dB into Ceiling
0 dBFS produces ~−0.96 dB of brickwall gain reduction and *zero* band GR on
the test fixture — there is nothing for ballistics to shape (§3.4's early-out
plus a barely-grazed brickwall).

Push it and the knobs demonstrably work:

- At **10 dB drive**, Dynamic attack 0→1 moves brick GR **−11.0 → −6.3 dB**
  with clipped samples going **0 → 0.21%** — texture change at constant
  loudness, with the hard clamp masking the leak.
- Release 0→1 swings post-peak recovery **99 ms → >900 ms**.
- Even mode ignores the knobs for the brickwall *by design* — the measured
  extremes diff at low drive is literally zero (−180 dB).

Two design findings worth keeping:

- **Enlarging the lookahead does not help.** Doubling `kBrickLA` to 512 did
  *not* reduce loose-mode clipping (0.21% → 0.24%) because `atk_samps` is a
  multiple of `kBrickLA` — the attack self-scales with the window, so the
  leak ratio is invariant. The actual lever for "softer attack without grit"
  is capping the coefficient (`0.15 + gain·1.05` → cap at `·0.80`) so max
  attack stays inside the window. Band-path lookahead was also evaluated and
  rejected: the solve ramp is 16–22 ms (dominated by the 5 ms attack
  smoothing) and one hop of lookahead covers only ~1/3 of it for +256 samples
  of latency.
- **MLG is dual-purpose** — water-fill α *and* brick attack character — which
  muddies the knob's identity in Dynamic mode.

The open plan (`docs/future/not-started/mel-limiter-default-gain-staging.md`,
issue #17): raise default MLD to 4–6 dB and/or drop MLC to −1 dBFS (owner
sound decision, `axon_meta.json` + `composite.py` in lockstep), optionally cap
the loose-attack coefficient. Acceptance explicitly expects the null-vs-old
render to fail — that is the point of a defaults change — so before/after
renders get recorded instead.

---

## 9. Honest limitations and open edges

- **Ill-conditioning is permanent.** The sort/branch solver and
  alignment-sensitive FFTs mean this stage will never pass a strict two-binary
  null. Steady-state-only validation is the accepted policy; don't burn time
  trying to do better, and don't let a warmup-region residual at −70 dBFS
  panic you — first check it against the documented no-change envelope.
- **Water-fill solver edge cases.** When even the quietest band exceeds the
  water level (the "all bands limited" partition, `k = −1`), the scan finds no
  acceptable cutoff and λ falls back to `g_uni` — which is a *gain ratio*, not
  a *level*, so the fallback is type-inconsistent (though clamped and benign in
  practice). Similarly `rem ≤ 0` collapses λ to 0. Both fire only on extreme,
  hot, flat spectra; a strictly-correct closed form for the k = −1 case would
  be `λ = C/√26`.
- **Sample-peak brickwall.** Inter-sample overs are TruePeakCeiling's job by
  architecture (§3.8), but anyone reading the "true-peak" comments in
  `mel_limiter.cpp:241-249` without that context will be misled.
- **The early-out makes the module feel dead at low drive** (§3.4, §8). A
  defaults change is planned; the deeper design question — should the spectral
  stage engage on something closer to peak than quasi-RMS? — is open.
- **MLG's dual identity** (spectral blend + Dynamic attack) is a knob-design
  smell; splitting it is the obvious refactor if the control surface ever gets
  revisited.
- **macOS arm64 only on main** (Accelerate vDSP); the `feat/xplat` branch is
  actively porting. Cross-platform validation will need the §7 policy plus a
  platform-independent steady-state oracle, since FFT backends won't agree
  bitwise.
- **Cosmetic staleness in the source:** the header banner still says latency is
  `kFFTSize` (`mel_limiter.hpp:9`), and `process()` carries a candid historical
  comment about an abandoned `run_hop_` refactor (`mel_limiter.cpp:383-388`) —
  the gain-apply/IFFT/OLA is simply inlined; behavior is exactly as described
  here.
- **Bench/bypass quirk:** the stage-timing "bypass" scenario omits MLI (default
  1.0), so MelLimiter runs hot in what looks like a bypass measurement — known
  quirk of the ranking harness, not a gating bug (`MLI = 0` genuinely skips
  the stage and drops its latency).

---

## 10. Verification map

`native/clap/tests/test_mel_limiter.cpp` — 19 units (built with `-UNDEBUG` so
asserts survive Release; the CMake targets add it explicitly because `-DNDEBUG`
would compile the whole suite into a no-op):

| Area | Tests |
|---|---|
| Basic hygiene | silence in→out; NaN/inf on extremes (DC, ±10 alternation, ±2 noise); reset clears state |
| Alignment | `wet_mix=0` is an *exact* `kLatency` dry delay; WOLA reconstruction wet≈dry <3% |
| Limiting | loud-signal RMS reduction; stereo link (hot L ducks quiet R); brickwall peak ≤ ceiling; drive increases loudness under a held ceiling; ceiling control audibility |
| Ballistics | Dynamic release slows recovery; Dynamic attack tight-vs-loose pre-duck; **lookahead THD ~0.1% vs clipper 22.7% (asserted < 0.4x)** |
| Rewrite regressions | chunked-vs-oneshot **exact 0.0**; fresh-instance identity **exact 0.0**; scratch sized by `init()` not first call |
| Contracts | `num_bands()`/mel centres vs independent HTK oracle (derived tolerance); `brickwall_gain()` trace (bounds, exact unity below ceiling via a Sterbenz argument, duck depth, recovery, bitwise determinism, stereo link); reset ≡ fresh instance bitwise; `copy_display()` state machine |

Performance is tracked by `bench/bench_mel_limiter.cpp` (stereo, drive 2x,
ceiling 0.5, blocks 64/128/512) and the `AXON_STAGE_TIMING` in-chain harness.
The in/out LUFS meters that visualize the limiter's effect are independently
cross-checked against the reference `LufsLeveler` in `tests/test_meter.cpp`.

## Superseded documents

- `docs/mel_limiter.md` (2026-06-05 tutorial) — algorithm pedagogy carried
  forward; its per-block-allocation gotcha, cost figures, default values
  (drive 0 dB / ceiling −1 dBFS / Even default), and most `file:line` anchors
  predate the vDSP rewrite and are corrected here.
- `native/clap/docs/limiter_algorithm.md` — the algorithm/control content is
  merged intact; its dense-solver description and 16-sample Even-attack figure
  are corrected.
- `native/clap/docs/mel_limiter_perf_findings.md` — merged wholesale into §7
  (variance envelope, reverted modulo candidate, steady-state-only policy,
  sample-peak note).

Still live elsewhere: `docs/future/not-started/mel-limiter-default-gain-staging.md`
(issue #17, pending owner decision) and `native/clap/docs/perf_stage_ranking.md`
(the cross-stage ranking that motivated §6).
