# Mel Limiter — Axon's multiband, mel-spaced spectral limiter

> A 26-band, perceptually-spaced (mel) STFT limiter that pulls each frequency
> region down only as much as it needs, followed by a true-peak brickwall, so
> the master can get loud without the bass squashing the highs (or vice versa).

This document is written for two readers at once. If you have **never touched
DSP**, read the "What is this?" and "Why it matters" sections and the prose in
each later section — they build the intuition from scratch. If you are a
**seasoned audio/DSP engineer**, the formulas, constants, and `file:line`
references give you everything precise. The two threads run in parallel; skim
whichever you need.

Source of truth:
- `native/clap/src/mel_limiter.hpp` — class, constants, state.
- `native/clap/src/mel_limiter.cpp` — the algorithm.
- `native/clap/src/axon_plugin.cpp` — how the plugin drives it (search
  `StageID::MelLimiter`, around line 1462).
- `native/clap/docs/limiter_algorithm.md` — the original algorithm notes; this
  file expands on them with a beginner-first framing. Cross-reference it for the
  control table and the "why this design" rationale.

---

## 1. What is this?

### A limiter, in plain English

Imagine you are mixing audio and you have a hard rule: the signal must **never
go above a certain level** (the *ceiling*). A naive way to enforce that is to
chop off anything sticking out above the line — that is *clipping*, and it
sounds harsh and distorted.

A **limiter** is the polite version. Instead of chopping, it *turns the volume
down* exactly when the signal would have exceeded the ceiling, and lets it back
up when the danger passes. Done well, you barely hear it working — you just hear
that the track got louder overall without ever poking through the ceiling. That
is the engine behind almost every "make it loud" mastering stage.

### "Broadband" vs "multiband"

A **broadband** limiter looks at the whole signal as one number. If a loud kick
drum hits, it turns *everything* down — kick, vocals, cymbals, all at once. You
hear the whole mix "duck" on every kick. That is the classic pumping sound.

A **multiband** limiter splits the audio into several **frequency bands** (think
of a graphic equalizer's sliders: lows, low-mids, mids, highs...) and limits
each band *independently*. Now a loud kick (low frequencies) only turns down the
low band; the vocals and cymbals in the higher bands keep playing at full
volume. The result is louder and less "pumpy," because only the offending region
gets reduced.

### "Mel-spaced"

How wide should each band be? Human hearing is not linear. We hear a huge amount
of detail in the low and low-mid range and progressively less resolution as we
go up. The **mel scale** is a perceptual frequency scale that reflects this: it
spaces bands close together in the bass and stretches them out in the treble, so
each band carries roughly equal *perceptual* importance. Putting the limiter's
bands on the mel scale means we spend our "resolution budget" where the ear
actually cares.

Axon uses **26 mel bands** spanning 20 Hz to 20 kHz.

### "Spectral" limiting (the STFT part)

Rather than building 26 real analog-style filters, Axon works in the **frequency
domain**. It repeatedly takes a short chunk of audio, runs a **Fourier transform
(FFT)** to see how much energy lives at each frequency, decides a gain for each
mel band, applies those gains directly to the spectrum, and transforms back to
audio. This is called **STFT** (Short-Time Fourier Transform) processing. It
gives extremely smooth, surgical, frequency-selective gain reduction that would
be expensive to build with real filters.

### The two-stage design

The mel/STFT stage controls **energy** (how loud each band is on average). But
energy is not the same as the instantaneous **peak** sample value — a signal can
have correct average energy yet still spike one sample above the ceiling. So
Axon follows the spectral stage with a small **true-peak brickwall** limiter
that guarantees no individual sample ever exceeds the ceiling. Two stages, two
jobs: spectral stage = perceptual loudness shaping; brickwall = absolute peak
safety.

---

## 2. Why it matters in mastering

Mastering is the final polish, and the dominant request is "make it loud and
competitive without sounding crushed." The Mel Limiter is Axon's loudness
maximizer:

- **Loudness without pumping.** Because each mel band limits independently, you
  can drive the overall level up far harder before it audibly squashes. A bass
  note no longer drags the whole track's level down.
- **Perceptual balance.** Mel spacing keeps the tonal balance natural as you
  push — the limiter does not preferentially eat the highs.
- **A character knob, not just a safety device.** The blend between "uniform"
  (transparent) and "water-filling" (aggressive, denser) and the
  attack/release controls let it go from invisible to glued-and-punchy.
- **A guaranteed ceiling.** The brickwall stage means the output literally
  cannot exceed the set ceiling, which is exactly what a master needs before it
  goes to a streaming platform's loudness normalization.

---

## 3. The DSP behind it

This section is the rigorous one. The architecture mirrors Newfangled Audio
*Elevate*:

```
analysis STFT → per-band energy → constrained gain solve →
time-smoothed gains → per-bin gain apply → IFFT + WOLA synthesis → brickwall
```

Key constants (`mel_limiter.hpp:21-25`):

| Constant | Value | Meaning |
|---|---|---|
| `kFFTSize` | 1024 | FFT/window length (the STFT frame) |
| `kHopSize` | 256 | samples between frames → 75% overlap |
| `kNumBands` | 26 | mel bands |
| `kBrickLA` | 256 | brickwall lookahead (~5.8 ms @ 44.1 kHz) |
| `kLatency` | 1280 | `kFFTSize + kBrickLA`, reported to host |

### 3.1 STFT analysis

Every input sample is multiplied by **Drive** and written into a circular
**analysis ring** (`mel_limiter.cpp:302`). Drive is applied here *only*, so
turning Drive up pushes the signal into the limiter without altering the dry
(bypass) path.

Once `kHopSize = 256` new samples have accumulated (`mel_limiter.cpp:308`), a
*hop* fires: the newest 1024 samples are linearized out of the ring, multiplied
by a **Hann window** (`mel_limiter.cpp:315-319`), and forward-transformed with
Apple Accelerate's real FFT, `vDSP_fft_zrip` (`mel_limiter.cpp:320-323`).

The Hann window (`mel_limiter.cpp:35-36`):

```
window[n] = 0.5 · (1 − cos(2π·n / N)),   N = 1024
```

With hop 256 / size 1024 there are **4 overlapping frames covering every output
sample** (75% overlap). That redundancy is what makes the reconstruction clean
(§3.7).

### 3.2 The mel filterbank (`build_mel_`, `mel_limiter.cpp:97-130`)

26 triangular bands are laid out on the **HTK mel scale** from `f_min = 20 Hz`
to `f_max = 20 kHz`. The HTK mel/Hz conversion:

```
mel(f) = 2595 · log10(1 + f/700)
f(mel) = 700 · (10^(mel/2595) − 1)
```

`kNumBands + 2 = 28` equally-spaced mel points are computed, converted back to
Hz, then to FFT-bin positions (`mel_limiter.cpp:103-108`). Each band `b` is a
triangle peaking at point `b+1`, ramping up from point `b` and down to point
`b+2` (`mel_limiter.cpp:119-123`):

```
band_to_bin[b][k] = max(0, min((k − lo)/ls, (hi − k)/rs))
```

Because the points are evenly spaced *in mel*, the triangles are narrow in the
bass and wide in the treble — perceptually even.

Two precomputed arrays drive everything downstream:

- `band_to_bin_[b·n_freq + k]` — weight of FFT bin `k` in band `b`.
- `bin_norm_[k] = Σ_b band_to_bin_[b][k]` — total band weight on each bin
  (`mel_limiter.cpp:126-129`), used later so a flat unity signal passes through
  untouched.

Band centre frequencies are stored in `band_center_hz_` for the UI x-axis
(`mel_limiter.cpp:116`). `n_freq_ = kFFTSize/2 + 1 = 513` (`mel_limiter.cpp:30`).

### 3.3 Per-band level — and the FFT normalization that makes it correct

For each band, the **energy** is the filterbank-weighted spectral power, taken
as the **maximum across channels** (linked stereo — decisions use the louder
channel so the stereo image is preserved). From `solve_gains_`
(`mel_limiter.cpp:143-156`):

```
e_b           = Σ_k  w_b[k] · |X[k]|²
band_level[b] = sqrt(e_b) · level_scale_
```

Note the DC and Nyquist bins are handled specially because `vDSP_fft_zrip`
packs the Nyquist real part into `im[0]` (`mel_limiter.cpp:149-152`).

**`level_scale_` is the single most important constant in the module.** Raw
vDSP FFT magnitudes are *not* normalized — they grow with the frame size `N` and
the window. Without correcting for this, `band_level` would read roughly `N`
times larger than the true signal amplitude, the solver would conclude *every*
signal is wildly over the ceiling, and it would **crush all audio to near
silence**. (That was a real historical bug — see Gotchas §8.)

The fix is derived analytically (`mel_limiter.hpp:110-114`,
`mel_limiter.cpp:40-44`). For a sine of amplitude `A`, the summed FFT-domain
energy through the filterbank is `A² · N · Σ window²`, so:

```
level_scale_ = 1 / sqrt(N · Σ window²)
```

`Σ Hann² over 1024 ≈ 384`, giving `level_scale_ = 1/sqrt(1024·384) ≈ 1/627`.
With it applied, a full-scale sine reads `total ≈ 1.0` — i.e. **band levels are
now in the same linear-amplitude units as the ceiling**, so every comparison to
`ceiling_lin` downstream is meaningful.

### 3.4 The constrained gain solve (`solve_gains_`, `mel_limiter.cpp:136-200`)

Let `C = ceiling_lin` and the broadband level be:

```
total = sqrt(Σ_b band_level[b]²)
```

**Early-out (`mel_limiter.cpp:165-168`).** If `total ≤ C` the frame is already
under the ceiling → all band gains = 1, no limiting. (Consequence: at low Drive
the spectral solver never engages and the brickwall does all the work — this is
why the adaptive controls can seem to "do nothing" until you push Drive.)

Otherwise it computes two candidate gain vectors and blends them.

**(a) Uniform (`mel_limiter.cpp:171`).** Reduce every band by the same ratio:

```
g_uni = C / total
```

This pulls `total` exactly to `C` while perfectly preserving the spectral
balance — transparent timbre, but a single loud band drags the whole mix down
(least loudness).

**(b) Reverse water-filling (`mel_limiter.cpp:177-191`).** Find the gains that
pull total energy down to `C²` while **only reducing bands that are too loud**
and leaving quiet bands untouched. Find a threshold `λ` such that:

```
Σ_n  min(1, λ/L[n])²  ·  L[n]²  =  C²
```

Bands with `L[n] ≤ λ` keep gain 1; bands with `L[n] > λ` are pulled down to `λ`
(gain `λ/L[n]`). It is the mirror image of classic water-filling: rather than
pouring power into the best channels, we *skim* power off the loudest bands until
the energy budget `C²` is met.

The solve is `O(N log N)`: sort the band levels ascending, then scan for the
cutoff `k` where bands `0..k` stay at unity (their accumulated energy is
`accum`) and the remaining `N−k−1` bands all sit at `λ`:

```
accum + (N−k−1)·λ²  =  C²   ⇒   λ = sqrt((C² − accum) / (N−k−1))
```

It is accepted at the first `k` where `sorted_L[k] ≤ λ ≤ sorted_L[k+1]`, which
proves the partition is self-consistent (band `k` truly below threshold, band
`k+1` above). If `accum` already exceeds `C²`, `λ → 0` (`mel_limiter.cpp:185`).

**(c) Blend (`mel_limiter.cpp:193-199`).**

```
g[n] = clamp( (1 − α)·g_uni + α·g_wf, 0, 1 ),   α = adaptive_gain
```

- `α = 0` → pure uniform → broadband, maximally transparent.
- `α = 1` → pure water-filling → multiband, denser/louder, can shift tone.

This is exactly what the **Adaptive Gain** knob does in the spectral stage.

### 3.5 Time-smoothing (attack / release)

Per-hop target gains would "zipper" (audible stepping) if applied raw, so each
band gain is one-pole smoothed toward its target once per hop
(`mel_limiter.cpp:333-338`):

```
coef          = (target < prev) ? atk_c : rel_c
band_gain[b]  = coef·prev + (1−coef)·target
```

The coefficients are computed per block from the hop length
(`mel_limiter.cpp:268-272`):

```
hop_ms = 1000 · kHopSize / sr            (≈ 5.8 ms @ 44.1 kHz)
atk_ms = 5                               (fixed fast attack)
rel_ms = 30 + adaptive_speed · 370       (30 … 400 ms)
atk_c  = exp(−hop_ms / atk_ms)
rel_c  = exp(−hop_ms / rel_ms)
```

Attack is fast (clamp down quickly when a band gets loud); **release is the
Adaptive Speed control**, ranging 30 ms (snappy) to 400 ms (slow, breathing).

### 3.6 Band → bin mapping (`mel_limiter.cpp:340-352`)

The 26 smoothed band gains are spread back onto the 513 FFT bins through the same
triangular weights, then normalized by `bin_norm_`:

```
bin_gain[k] = ( Σ_b w_b[k]·band_gain[b] ) / bin_norm[k]
```

The `/bin_norm[k]` is what guarantees that if all band gains are 1, every bin
gain is exactly 1 — i.e. **a flat unity signal passes through untouched** (no
coloration when the limiter is idle). Where `bin_norm[k]` is ~0 (no band covers
the bin), gain defaults to 1 (`mel_limiter.cpp:347-351`).

### 3.7 Apply, IFFT, and weighted overlap-add (WOLA)

The already-computed spectrum is multiplied by `bin_gain` in place — again
handling the packed DC bin (`realp[0]`) and Nyquist bin (`imagp[0]`) specially
(`mel_limiter.cpp:368-373`) — then inverse-FFT'd (`mel_limiter.cpp:376-378`),
multiplied by the Hann **synthesis** window again, and scaled by:

```
ola_scale_ = 1 / (2·N)
```

which is the exact inverse of vDSP's `zrip` forward+inverse round-trip gain of
`2N` (`mel_limiter.cpp:31`, `383-384`).

The windowed frame is **overlap-added** into `out_ring`; in parallel, `window²`
is overlap-added into `norm_ring` (`mel_limiter.cpp:389-398`). The output sample
is the normalized ratio (`mel_limiter.cpp:411-414`):

```
wet = out_ring[r] / norm_ring[r]
```

This is **WOLA (weighted overlap-add) normalization**: the signal accumulates
`frame·window²` and the denominator accumulates `window²`, so dividing recovers
the signal regardless of how the windows sum. With 75% overlap `Σ window² → 1.5`
(constant), and a unity-gain signal reconstructs **exactly** (verified to 0%
error in the unit tests).

Finally, a **1-sample `wet_z1` delay** (`mel_limiter.cpp:423-425`) trims the
WOLA path's natural `kFFTSize − 1` group delay up to exactly `kFFTSize`, so it
aligns perfectly with the dry delay ring and the reported latency. (Getting this
off by one sample was the second historical bug — see §8.)

### 3.8 The true-peak brickwall (`brickwall_`, `mel_limiter.cpp:220-256`)

The spectral stage controls energy, not instantaneous peaks, so a final linked
lookahead limiter pins the actual sample peak to the ceiling:

- A `kBrickLA = 256`-sample (~5.8 ms) lookahead line delays the audio.
- The gain targets the **loudest sample anywhere in the lookahead window**,
  found with a **sliding-window maximum** implemented as a monotonic deque
  (`dq_val_`/`dq_idx_`, `mel_limiter.cpp:229-241`), which is `O(1)` amortized.
  The point of the window (vs a naive single-sample tap) is that the gain *sees
  the worst upcoming peak the moment it enters the window* and has the full
  window to ramp down for it.
- `g_req = C / windowed_max` when that max exceeds `C`, else 1
  (`mel_limiter.cpp:242-243`); smoothed with attack/release into `brick_gain_`
  (`mel_limiter.cpp:246-247`).
- The delayed sample is multiplied by `brick_gain_`, then **hard-clamped to ±C**
  (`mel_limiter.cpp:249-254`). That clamp is the absolute guarantee: regardless
  of ballistics, `|out| ≤ C`.

The window + windowed detector means the gain is fully down before a peak in the
clean modes, so the clip barely fires → low distortion, especially on **bass
transients** whose long wavelengths a short window would clip rather than duck.
(Measured in tests: ~0.1% THD on a 60 Hz tone vs ~22.7% for a pure clipper.)

#### Even vs Dynamic (the brickwall character)

Set per block (`mel_limiter.cpp:282-290`):

- **Even** (`adaptive_brickwall = false`): fixed tight attack (`kBrickLA·0.25`
  ≈ 64-sample time constant) + fast 50 ms release. Consistent, clean,
  transparent.
- **Dynamic** (`adaptive_brickwall = true`): the adaptive knobs reshape the
  brickwall:
  - **Adaptive Gain → attack character**:
    `atk_samps = kBrickLA·(0.15 + adaptive_gain·1.05)`. Tight (~38 smp, fully
    pre-ducks well inside the 256-smp lookahead → clean) up to loose (~307 smp,
    *slower than the lookahead* so transients partially leak and the hard clip
    catches them → punch + a little clipper grit).
  - **Adaptive Speed → release**: `50 + adaptive_speed·350` ms (50–400 ms; slow
    = breathing/pumping).

Because attack is always bounded by the lookahead and the hard clip always fires
last, "loose" trades transient clipping for punch *without ever breaking the
ceiling*.

---

## 4. How it works in the code (walkthrough)

### Public surface (`mel_limiter.hpp`)

- `init(int sample_rate)` (`mel_limiter.cpp:22-68`) — builds the FFT setup, Hann
  window, `level_scale_`, per-channel rings, brickwall ballistics, the mel
  filterbank, then `reset()`.
- `reset()` (`mel_limiter.cpp:70-91`) — zeros all rings/state, band gains → 1,
  brickwall gain → 1. Real-time safe (no allocation).
- `process(float* l, float* r, int n_ch, int n_samples, const Params& p)`
  (`mel_limiter.cpp:262-444`) — the in-place processing entry point, up to 2
  channels.
- `copy_display(levels, gains, centers)` (`mel_limiter.cpp:202-209`) — snapshots
  per-band measured levels, applied gains, and centre frequencies for the UI.
- `brickwall_gain()` (`mel_limiter.hpp:60`) — current peak-limiter gain for the
  meter.

### The streaming contract (`process`)

`process` is the streaming heart. Per input sample (`mel_limiter.cpp:295-443`):

1. Write the raw sample to the **dry delay ring** and `sample·drive_lin` to the
   **analysis ring** (`mel_limiter.cpp:301-304`).
2. When 256 samples have accumulated, run a **hop** (`mel_limiter.cpp:308`):
   forward FFT per channel → `solve_gains_` → time-smooth band gains → map to bin
   gains → apply gains → IFFT → window + scale → overlap-add into the output and
   norm rings (`mel_limiter.cpp:311-401`).

   > Implementation note: lines `354-363` carry a candid comment about an
   > abandoned `run_hop_` refactor; the gain-apply/IFFT/OLA is inlined here
   > instead. The behavior is exactly as described; the comment is historical.

3. Every sample, drain one **aligned wet** sample from the output ring (with the
   WOLA `/norm` normalization and the 1-sample `wet_z1` alignment delay)
   (`mel_limiter.cpp:405-426`).
4. Feed the wet sample through `brickwall_` (`mel_limiter.cpp:428-430`).
5. Read the dry sample delayed by exactly `kLatency` and output the wet/dry
   blend (`mel_limiter.cpp:432-442`):

   ```
   out = (1 − wet_mix)·dry + wet_mix·wet_out
   ```

### Buffering & alignment

- **Analysis ring** `in_ring` — size `kFFTSize`, circular, holds the latest
  driven samples for the FFT.
- **Output rings** `out_ring` / `norm_ring` — size `kFFTSize + kHopSize`, hold
  the overlap-added wet signal and its window² normalizer.
- **Dry ring** `dry_ring` — size `kLatency + kHopSize`, delays the clean signal
  by exactly `kLatency` so the wet/dry blend is sample-aligned.
- **Lookahead ring** `la_ring` — size `kBrickLA`, delays the wet signal for the
  brickwall.

Stereo is **linked**: band decisions use the per-band max across channels
(`mel_limiter.cpp:144-154`) and the brickwall uses the max magnitude across
channels (`mel_limiter.cpp:223-224`), so both channels receive identical gain
and the stereo image is preserved.

### Plugin integration (`axon_plugin.cpp`)

In `resolve_amount_` the host control values are converted to limiter params
(`axon_plugin.cpp:1160-1165`):

```
ml_wet            = MLI                                  // wet/dry mix
ml_ceiling_lin    = 10^(MLC/20)   // MLC is dBFS  → linear amplitude
ml_drive_lin      = 10^(MLD/20)   // MLD is dB    → linear gain
ml_adaptive_gain  = MLG
ml_adaptive_speed = MLS
ml_adaptive_brickwall = (MLA ≥ 0.5)   // EVEN vs DYNAMIC toggle
```

The `StageID::MelLimiter` case (`axon_plugin.cpp:1462-1474`) fills a
`MelLimiter::Params`, early-outs when `ml_wet ≤ 0`, and calls `process` on the
working buffers. The limiter is `init`'d at activation
(`axon_plugin.cpp:959`) and `reset` on deactivation/reset
(`axon_plugin.cpp:1077`). Display state is snapshotted on the main-thread
callback (`axon_plugin.cpp:1057`, `1707-1712`).

---

## 5. Latency & performance

- **Exact latency: `kLatency = kFFTSize + kBrickLA = 1024 + 256 = 1280
  samples`** (~29 ms @ 44.1 kHz, ~26.7 ms @ 48 kHz). The STFT path contributes
  `kFFTSize` (after the 1-sample `wet_z1` trim) and the brickwall adds
  `kBrickLA`; the dry ring is delayed by the same total so wet and dry stay
  aligned.
- The plugin **reports this to the host** for delay compensation, and *only when
  the limiter is enabled* (`axon_plugin.cpp:858-859`):
  ```
  if (ml_wet > 0) lat += MelLimiter::kLatency;
  ```
- **FFT backend:** Apple Accelerate **vDSP** (`vDSP_fft_zrip`, real split-radix),
  macOS arm64 only (`mel_limiter.hpp:8,12`). The FFT setup is created once in
  `init`.
- **Real-time safety:** `process` and `reset` perform **no heap allocation**;
  all rings and scratch buffers are sized in `init`. The one exception is a
  per-call `std::vector<float> bin_gain_arr(n_freq_)` at
  `mel_limiter.cpp:293` — a per-block (not per-sample) scratch allocation; on a
  strict RT-audit this is the one thing to hoist into member state.
- **Cost:** one forward + one inverse 1024-pt FFT per channel per 256-sample hop,
  plus an `O(N log N)` band sort (N = 26, trivial). The brickwall's
  sliding-window max is `O(1)` amortized per sample.

---

## 6. Parameters

The host-facing controls (IDs in parentheses); the live numeric min/max/default
come from the loaded model bundle's control metadata
(`plug->meta->controls`), but the *semantics and internal ranges* are fixed in
code as follows:

| Control (ID) | Internal param | Meaning | Range / mapping |
|---|---|---|---|
| **Drive** (MLD) | `drive_lin` | Input gain into the analysis path — pushes the signal into the ceiling for loudness. Does **not** touch the dry path. | dB → `10^(MLD/20)` linear; default 0 dB (×1) |
| **Ceiling** (MLC) | `ceiling_lin` | Target `C` for the spectral solve **and** the hard `±C` clamp of the brickwall. | dBFS → `10^(MLC/20)` linear; default −1 dBFS |
| **Adaptive Gain** (MLG) | `adaptive_gain` (α) | Spectral: blends uniform (transparent) ↔ water-fill (aggressive). Dynamic brickwall: sets attack character. | 0…1; default 0.5 |
| **Adaptive Speed** (MLS) | `adaptive_speed` | Spectral release 30→400 ms. Dynamic brickwall release 50→400 ms. | 0…1; default 0.5 |
| **Dynamic** (MLA) | `adaptive_brickwall` | Brickwall mode: **EVEN** (fixed tight/fast) vs **DYNAMIC** (MLG/MLS reshape it). | boolean; on when MLA ≥ 0.5; default EVEN |
| **Limiter** (MLI) | `wet_mix` | Wet/dry mix of the whole module. At 0 the stage is skipped (and its latency is dropped). | 0…1; default 1 |

Note the dual role of `adaptive_gain` and `adaptive_speed`: they *always* shape
the spectral stage; in **DYNAMIC** mode they *additionally* reshape the brickwall
attack/release.

---

## 7. Where it sits in the Axon chain & related modules

The Mel Limiter is the reorderable stage `StageID::MelLimiter`
(`axon_plugin.cpp:1462`). It is the plugin's **loudness maximizer**, typically
placed late in the chain after tone-shaping (saturator, auto-EQ, SSL comp) so it
maximizes the final, balanced signal.

### Signal flow within the module

```
            drive                STFT          per-hop solve       per-bin       IFFT + WOLA
 in ──┬─► ×drive ─► analysis ─► FFT ─► 26 mel ─► constrained ─► gain ─► apply ─► overlap-add ─► +1 smp ─► brickwall ─┬─► × wet
      │             ring               levels    gain (α blend)  map            synthesis       align    (lookahead) │
      └─► dry delay ring ───────────────────────────────────────────────────────────────────────────────────────────┴─► × (1−wet) ─► out
```

### Two independent ceilings — don't confuse them

1. **The brickwall clip inside this module**, at the limiter's own `ceiling_lin`
   (MLC). This is the maximizer's output cap.
2. **`TruePeakCeiling`** — a *separate*, always-on, plugin-level stage that runs
   dead last in the chain (`axon_plugin.cpp:1499-1511`), 4×-oversampled true
   peak, default ~−1 dBTP. It is the whole-signal-path safety net (catches
   overshoot from *any* module) and is independent of the limiter.

### Related modules

- **TruePeakCeiling / Trim** — the final, non-reorderable safety limiter and
  trim gain (`axon_plugin.cpp:1499-1511`).
- **LoudnessMeter** — the in/out LUFS/RMS/Peak meters; the OUT meter reads the
  real master *after* the limiter so driving the limiter shows the actual target
  loudness (`axon_plugin.cpp:1513-1528`).
- **Saturator, AutoEQ, SSL Comp, BassMono** — the tone/dynamics stages that
  precede the limiter in a typical chain.

---

## 8. Gotchas / things to watch

- **FFT energy normalization (`level_scale_`) is load-bearing.** This was a real
  historical bug: without it, raw vDSP magnitudes read ~`N`× too high, the solver
  thought everything was massively over the ceiling, and the limiter **crushed
  all audio**. The fix `level_scale_ = 1/sqrt(N·Σwindow²)` puts band levels in
  the same linear-amplitude units as the ceiling. If you ever change the window
  or FFT size, this constant must be recomputed (it is, automatically, in
  `init`) — and any custom band-level math must keep the scaling.
- **The 1-sample latency alignment (`wet_z1`).** The WOLA path's natural group
  delay is `kFFTSize − 1`; the single-sample `wet_z1` delay bumps it to exactly
  `kFFTSize` so it matches the reported `kLatency` and the dry ring. This was the
  second historical bug. If wet and dry ever sound "phasey" or combed when mixed,
  suspect a latency-alignment regression here (`mel_limiter.cpp:421-425`).
- **The spectral solver is silent at low Drive.** When `total ≤ C` it early-outs
  with unity gains, so Adaptive Gain/Speed appear to "do nothing" until you push
  Drive enough to exceed the ceiling. The brickwall is still active and does the
  peak work. This is by design, not a bug.
- **Latency only counts when enabled.** Latency is reported only if `MLI > 0`
  (`axon_plugin.cpp:858-859`); toggling the limiter changes the plugin's reported
  latency, so hosts may need to re-scan PDC. Setting `MLI = 0` fully skips the
  stage (`axon_plugin.cpp:1463`).
- **macOS/arm64 only.** The FFT is Apple Accelerate vDSP. Porting off macOS
  requires swapping the FFT backend (and re-checking the `zrip` packing of the
  Nyquist bin into `im[0]`, used in `solve_gains_` and the gain-apply loop).
- **DYNAMIC "loose" attack deliberately leaks into the clip.** With high Adaptive
  Gain in DYNAMIC mode the brickwall attack (~307 smp) is slower than the
  lookahead (256 smp), so transients partially escape the smooth gain and hit the
  hard clamp. That is intentional (punch + grit), and the clamp still guarantees
  `|out| ≤ C` — but expect measurably higher THD there than in EVEN/tight modes.
- **One per-block allocation.** `bin_gain_arr` is allocated each `process` call
  (`mel_limiter.cpp:293`). Harmless in practice (per block, not per sample) but
  worth hoisting if you do strict no-allocation RT auditing.

---

## Verification

The behavior described here is covered by `native/clap/tests/test_mel_limiter.cpp`
(silence, exact dry-bypass delay, ceiling reduction, NaN/extremes, reset, stereo
link, **0% WOLA reconstruction**, brickwall peak cap, drive loudness, ceiling
audibility, adaptive-brickwall attack/release, and the **lookahead
low-distortion** test: ~0.1% THD vs ~22.7% for a pure clipper on a 60 Hz tone).
The in/out LUFS meter is cross-checked against the reference `LufsLeveler` in
`native/clap/tests/test_meter.cpp`.
