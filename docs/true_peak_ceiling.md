# True-Peak Ceiling

> The final, non-negotiable safety limiter in the Axon chain: a 4× oversampled,
> lookahead, inter-sample peak limiter that guarantees the output never exceeds a
> configured ceiling (default **−1 dBTP**) as measured by a true-peak meter.

---

## 1. What is this?

Imagine you record a sound and store it on a computer. The computer doesn't store
a continuous wave — it stores a long list of numbers, one every tiny fraction of a
second. Each of those numbers is a **sample**. If you graph the samples as dots,
the original smooth sound wave is somewhere passing *through* those dots.

When the audio is played back, a chip called a **DAC** (digital-to-analog
converter) reconnects the dots back into a smooth wave. Here's the catch: the
smooth wave that runs through the dots can **bulge higher than any individual
dot**. The peak of the real, reconstructed wave can sit *between* two samples,
where you never stored a number.

```
   sample peak (highest dot)              TRUE peak (the curve between dots)
        |                                          |
        v                                          v
   *         *                              *  __  *
    \       /                                \ /  \ /
     \     /                                  X    X   <- the real wave overshoots
      \   /                                  / \  / \     the dots here
       \ /                                  *   ``   *
        *
   "sample peak" only looks at the dots    "true peak" looks at the curve
```

- **Sample peak**: the largest absolute value among the stored numbers. Easy to
  measure, but it *lies* — it ignores the bulges between samples.
- **True peak** (a.k.a. **inter-sample peak**): the largest value of the actual
  *reconstructed* analog wave, including the bulges between samples.

### Why does this matter?

A digital signal can have every single sample at or below "full scale"
(0 dBFS, the maximum a sample can hold) and yet, when the DAC reconstructs the
wave, the curve between samples shoots **above** full scale. Downstream this
causes problems:

- The DAC's analog output stage clips (distorts).
- Lossy encoders (MP3, AAC, Opus) shift sample positions slightly during
  encode/decode; what looked safe at the sample level now clips on playback.
- Sample-rate conversion (e.g. 44.1 kHz → 48 kHz on a phone) re-draws the curve
  and can expose new overshoots.

### dBTP

True peak is measured in **dBTP** — decibels relative to full scale, *true peak*.
- `0 dBTP` = the reconstructed wave just touches full scale.
- `−1 dBTP` = the reconstructed wave peaks 1 dB *below* full scale (≈ 89% of max).
- Positive dBTP values mean the wave overshoots full scale (bad).

The standard way to *estimate* true peak (per ITU-R BS.1770) is to **oversample**
the signal — synthetically compute extra samples *between* the real ones, so the
bulges show up as actual dots you can measure. Axon's ceiling does exactly this,
at 4×.

### Why streaming platforms care

Spotify, Apple Music, YouTube, etc. normalize loudness and **require headroom**
to avoid clipping after their own processing. Apple's "Mastered for iTunes"
guidance, for example, calls for a true-peak ceiling no higher than −1 dBTP.
Delivering a master that respects a true-peak ceiling is the difference between
"plays clean everywhere" and "crackles on someone's phone."

---

## 2. Why it matters in mastering

In mastering, the **last** thing in the chain should be an absolute guarantee:
"no matter what the creative stages did, the output will not exceed X dBTP."

Axon's earlier stages (the auto-EQ, the saturator, the SSL-style bus comp, and
especially the MelLimiter) are *musical* — they shape tone and dynamics, and they
can, individually or in combination, overshoot a peak target. The True-Peak
Ceiling is the deterministic **backstop**. As the header puts it:

> "This is a deterministic backstop for the TONE chain — the LA-2A comp and
> saturator can overshoot peak targets; this stage makes the ceiling
> non-negotiable without coloring the dynamics."
> — `true_peak_ceiling.hpp:8`

The **−1 dBTP convention** is baked in as the default ceiling
(`true_peak_ceiling.hpp:25`, and the shipped bundle `axon_meta.json:263`).
That 1 dB of headroom is the industry-standard safety margin described above.

---

## 3. The DSP behind it

There are four ideas stacked together. We'll build each one up.

### 3a. Oversampling for inter-sample detection

To "see" the bulges between samples, we reconstruct extra samples in between. The
classic recipe is **zero-stuffing + lowpass filtering**:

1. Insert 3 zeros after every input sample (this is what makes it 4× — "zero
   stuffing", `kOvsFactor = 4`, `true_peak_ceiling.hpp:44`).
2. Run that stuffed stream through a lowpass FIR filter whose cutoff is the
   original Nyquist frequency. Mathematically, this interpolates — it fills the
   zeros with the values the smooth reconstructed wave would have there.

The result is 4 output values for every 1 input value. The largest absolute
value among those 4 is our **true-peak estimate** for that input sample. 4×
oversampling is the BS.1770-4 reference rate and catches inter-sample peaks to
within a few hundredths of a dB for normal program material.

### 3b. The polyphase trick

Naively, zero-stuffing then filtering wastes work — 3 out of every 4 multiplies
are "multiply by zero." A **polyphase** decomposition reorganizes the single
32-tap filter into 4 sub-filters ("phases") of 8 taps each
(`kFirTaps = 32`, `kFirPhase = 8`, `true_peak_ceiling.hpp:45-46`). Each phase
computes one of the 4 oversampled outputs directly from the real input samples,
no zeros multiplied. Same math, 4× less work.

### 3c. Peak detection → gain computation

Once we have the 4× peak magnitude for the current sample (`peak_mag`), the
desired gain is trivial:

- If `peak_mag` is at or below the ceiling, do nothing (gain = 1.0).
- If `peak_mag` exceeds the ceiling, scale it down by exactly the right amount:
  `gain = ceiling / peak_mag`.

```cpp
double target_gr = 1.0;
if (peak_mag > ceiling_lin_) {
    target_gr = ceiling_lin_ / peak_mag;   // true_peak_ceiling.cpp:106-108
}
```

This is a pure *peak* limiter law — the gain needed is precisely the ratio that
brings the offending peak down to the ceiling.

### 3d. Lookahead

If a loud peak arrives suddenly, a limiter that reacts *at* the peak is already
too late — it has to either let the peak through or slam the gain down
instantly (which distorts). **Lookahead** fixes this: delay the *audio* by a
small amount, but compute the gain from the *un-delayed* (future) signal. By the
time the loud sample actually reaches the output, the gain reduction has already
had time (the "attack" time) to ramp in smoothly.

Axon delays the audio by `lookahead_ms` (default **1.5 ms**, ≈ 66 samples at
44.1 kHz, `true_peak_ceiling.hpp:27`). It computes the gain from the current
sample's true-peak estimate, but applies it to a sample that is
`lookahead_samples_` older. So the gain control "sees" each peak exactly that far
in advance.

### 3e. Attack / release smoothing

Switching gain abruptly creates clicks and distortion. The gain is smoothed with
a one-pole filter that has two different speeds:

- **Attack** (default 0.5 ms): how fast the gain *clamps down* when a peak
  appears. Fast, so it catches the peak within the lookahead window.
- **Release** (default 50 ms): how fast the gain *recovers* after the peak
  passes. Slow, to avoid audible "pumping."

The one-pole coefficient for a time constant τ is `exp(-1 / (τ · Fs))`
(`true_peak_ceiling.cpp:63-66`).

### 3f. The hard-clip safety net

Smoothing means the gain can momentarily lag a hair behind a very fast peak. To
make the ceiling *truly* non-negotiable, the final output is hard-clipped to the
ceiling as a last resort (`true_peak_ceiling.cpp:119-120`). In practice the
lookahead + attack handle peaks transparently, and this clip almost never fires —
but it's there so the guarantee is absolute.

### Signal flow

```
        in[i] ──► FIR history ──► 4× polyphase upsample ──► |max of 4| = peak_mag
          │                                                       │
          │                                              gain = ceiling / peak_mag
          │                                                       │
          │                                          one-pole smoother (atk/rel)
          │                                                       │
          ▼                                                       ▼
   lookahead delay line ─────────────────► (×) ──► hard-clip [-ceil,+ceil] ──► out[i]
   (delayed audio)                          gain
```

---

## 4. How it works in the code

Two files, both in `native/clap/src/`:
`true_peak_ceiling.hpp` (interface + config) and `true_peak_ceiling.cpp`
(implementation). The class is pure C++ in namespace `nablafx`, with no CLAP or
ONNX-Runtime dependencies, so it unit-tests standalone
(`true_peak_ceiling.hpp:12`).

### Configuration (`true_peak_ceiling.hpp:24-29`)

```cpp
struct Config {
    double ceiling_dbtp = -1.0;  // hard ceiling measured 4×
    double lookahead_ms = 1.5;   // at 44.1k ≈ 66 samples
    double attack_ms    = 0.5;   // gain-reduction time constants
    double release_ms   = 50.0;
};
```

### Building the FIR — `build_half_band_fir` (`true_peak_ceiling.cpp:15-42`)

The interpolation filter is designed at `reset()` time as a **windowed-sinc**
lowpass:

- Cutoff `fc_norm = 0.5 / 4 = 0.125` in oversampled-normalized units — i.e. the
  original Nyquist (`true_peak_ceiling.cpp:20`).
- An ideal sinc, evaluated over 32 taps centered at `(N-1)/2`
  (`true_peak_ceiling.cpp:23-27`).
- Multiplied by a **Hann window** to tame ringing
  (`true_peak_ceiling.cpp:29`).
- Normalized so the tap sum equals the oversample factor (4), which makes the
  effective per-phase DC gain unity after polyphase recombination
  (`true_peak_ceiling.cpp:39-41`).

### `reset(sample_rate)` (`true_peak_ceiling.cpp:46-69`)

Called when the host activates the plugin (and on sample-rate change). It:

1. Builds the FIR (`:48`).
2. Computes `lookahead_samples_ = round(lookahead_ms · 1e-3 · Fs)`, clamped to a
   minimum of 1 (`:50-52`).
3. Allocates and zeroes the lookahead delay line to that length (`:54`). **All
   allocation happens here, never in `process`.**
4. Zeroes the FIR history ring (`:57-58`).
5. Converts the dBTP ceiling to linear:
   `ceiling_lin_ = 10^(ceiling_dbtp / 20)` (`:60`).
6. Resets the smoothed gain `gr_lin_` to 1.0 (`:61`).
7. Computes attack/release one-pole coefficients (`:63-68`).

### `process(in, out, n)` (`true_peak_ceiling.cpp:71-124`)

Runs per channel, per sample (`process` is documented as **in-place safe** —
`in` and `out` may alias, `true_peak_ceiling.hpp:36`). For each input sample:

1. **Push into the FIR history ring** (`:79-80`):
   ```cpp
   fir_hist_[fir_hist_idx_] = x;
   fir_hist_idx_ = (fir_hist_idx_ + 1) % ph;   // ph = 8
   ```

2. **Polyphase upsample + peak detect** (`:86-97`). For each of the 4 phases `p`,
   convolve the 8 taps `tap = p + k*4` against the 8 most-recent inputs, and keep
   the largest absolute result:
   ```cpp
   for (std::size_t p = 0; p < fac; ++p) {            // fac = 4
       double acc = 0.0;
       for (std::size_t k = 0; k < ph; ++k) {         // ph = 8
           std::size_t tap   = p + k * fac;           // 0..31
           std::size_t h_idx = (fir_hist_idx_ + ph - 1 - k) % ph;
           acc += fir_[tap] * fir_hist_[h_idx];
       }
       double mag = std::abs(acc);
       if (mag > peak_mag) peak_mag = mag;
   }
   ```

3. **Gain computation** (`:105-108`): `target_gr = ceiling_lin_ / peak_mag` only
   if the 4× peak exceeds the ceiling, else 1.0.

4. **Gain smoothing** (`:110-111`): pick attack coeff if the new target is
   *lower* than the current gain (clamping down), release coeff otherwise, then
   one-pole filter:
   ```cpp
   double coeff = (target_gr < gr_lin_) ? attack_coeff_ : release_coeff_;
   gr_lin_ = coeff * gr_lin_ + (1.0 - coeff) * target_gr;
   ```

5. **Lookahead delay + apply gain + safety clip** (`:114-122`):
   ```cpp
   float delayed = delay_[delay_idx_];          // read the old sample
   delay_[delay_idx_] = static_cast<float>(x);  // write the current one
   delay_idx_ = (delay_idx_ + 1) % delay_.size();

   double y = delayed * gr_lin_;                 // apply smoothed gain
   if (y > ceiling_lin_)  y = ceiling_lin_;      // hard-clip safety net
   if (y < -ceiling_lin_) y = -ceiling_lin_;
   out[i] = static_cast<float>(y);
   ```
   The gain derived from the *current* (future) peak is applied to a sample that
   is `lookahead_samples_` older — that's the lookahead in action
   (`:99-104` explains this alignment in the source comment).

### Reported latency (`true_peak_ceiling.hpp:40`)

```cpp
std::size_t latency_samples() const { return lookahead_samples_; }
```

The module exposes exactly its lookahead so the plugin can report it to the DAW.

---

## 5. How it's wired into the Axon plugin

All references below are in `native/clap/src/axon_plugin.cpp`.

### Position in the chain

The ceiling is the **final** processing stage, applied *after* the output trim
gain and *not* user-reorderable (`axon_plugin.cpp:1499-1511`):

```cpp
// Trim + TruePeakCeiling — always last, not user-reorderable. This is the
// REAL master; the OUT meter reads it ...
if (amt.trim_lin != 1.f) { /* apply trim to work_l / work_r */ }
for (uint32_t ch = 0; ch < n_ch; ++ch) {
    float* blk = (ch == 0) ? work_l : work_r;
    plug.chains[ch].ceiling.process(blk, plug.chains[ch].out_buf.data(), kBlockSize);
    ...
}
```

It runs **per channel** — each chain holds its own `TruePeakCeiling ceiling;`
instance (`axon_plugin.cpp:611`, "TruePeakCeiling runs per-channel"
`:563`). The OUT meter reads the ceiling's output (so pushing the limiter shows
the true master loudness), and Auto Gain is applied *after* metering.

### Construction & reset

On activation, each channel's ceiling is built from the bundle's JSON metadata
(`axon_plugin.cpp:1034-1041`):

```cpp
TruePeakCeiling::Config tcfg{
    /*ceiling_dbtp=*/g_state->axon_meta.ceiling.ceiling_dbtp,
    /*lookahead_ms=*/g_state->axon_meta.ceiling.lookahead_ms,
    /*attack_ms=*/g_state->axon_meta.ceiling.attack_ms,
    /*release_ms=*/g_state->axon_meta.ceiling.release_ms,
};
ch.ceiling = TruePeakCeiling(tcfg);
ch.ceiling.reset(sample_rate);
```

Those values are loaded from `weights/axon_bundle/axon_meta.json` (the
`"ceiling"` object, lines 262-267 of that file), parsed in
`composite_meta.cpp:96-100` into `CompositeCeilingCfg`
(`composite_meta.hpp:35-39`). The shipped defaults match the header:
`ceiling_dbtp = -1.0`, `lookahead_ms = 1.5`, `attack_ms = 0.5`,
`release_ms = 50.0`.

> **Note:** the True-Peak Ceiling is **not** a live, host-exposed knob. It is a
> fixed safety stage configured by the model bundle. The two host-facing
> composite knobs are AMT (amount) and TRM (trim). Do not confuse the ceiling
> with the **MelLimiter**'s `MLC` ceiling parameter — that's a separate, musical
> limiter earlier in the chain (see Related modules).

---

## 6. Latency & performance

### Lookahead latency (exact)

The module's latency equals its lookahead in samples:

```
lookahead_samples_ = round(lookahead_ms × 1e-3 × Fs)   // min 1
```

With the default 1.5 ms:

| Sample rate | Lookahead samples | Latency |
|-------------|-------------------|---------|
| 44.1 kHz    | round(66.15) = **66** | ~1.50 ms |
| 48 kHz      | round(72.0)  = **72** | 1.50 ms |
| 96 kHz      | round(144.0) = **144**| 1.50 ms |

The plugin folds this into its end-to-end latency report. From
`compute_latency_` (`axon_plugin.cpp:828-832`):

```cpp
uint32_t lat = kBlockSize;                                  // 128-sample accumulator
lat += static_cast<uint32_t>(plug.chains[0].ceiling.latency_samples());
```

So the ceiling's lookahead is **always present once activated** (header comment,
`axon_plugin.cpp:827`), added on top of the unconditional 128-sample block
accumulator. Other stages (spectral EQ, SSL comp, MelLimiter) add latency only
when active. The DAW uses this number to compensate (delay other tracks so
everything stays aligned).

### Compute cost

Per output sample the inner work is `kOvsFactor × kFirPhase = 4 × 8 = 32`
multiply-accumulates for the upsample/peak detector, plus a handful of scalar ops
for the gain math — i.e. one 32-tap FIR's worth of work per sample, per channel.
This is cheap and constant.

### Real-time safety

- **No allocation in `process`** — the delay line and FIR history are sized once
  in `reset` (`true_peak_ceiling.cpp:54, 57`). The hot path only does arithmetic
  and ring-buffer indexing.
- **No locks, no syscalls, no exceptions** in `process`.
- Deterministic, branch-light, in-place safe.

---

## 7. Parameters

These come from the model bundle JSON (`axon_meta.json` → `"ceiling"`), not from
host automation. Defaults shown.

| Param | Default | Units | Meaning |
|-------|---------|-------|---------|
| `ceiling_dbtp` | **−1.0** | dBTP | The absolute true-peak ceiling. Output (4× measured) will not exceed this. Converted to linear `ceiling_lin_ = 10^(dBTP/20)` (≈ 0.891 at −1 dBTP). |
| `lookahead_ms` | **1.5** | ms | How far ahead the gain control "sees" peaks. Also the module's reported latency. Larger = smoother limiting, more delay. |
| `attack_ms` | **0.5** | ms | How fast gain clamps down on a peak. Must be ≤ lookahead so reduction fully engages before the peak reaches the output. |
| `release_ms` | **50.0** | ms | How fast gain recovers after a peak. Larger = smoother (less pumping) but more sustained loudness loss after transients. |

Practical notes:

- **Lower the ceiling** (e.g. −2 dBTP) for extra delivery headroom; **raise it**
  (toward 0) for maximum loudness at higher inter-sample-clip risk on lossy
  codecs.
- The released gain always returns toward 1.0 (unity) — this stage adds **no**
  makeup gain. It only ever attenuates.

---

## 8. Gotchas / things to watch

- **Distortion vs. transparency.** The lookahead (1.5 ms) and fast attack
  (0.5 ms) are tuned so peak reduction is smooth and the audible hard-clip almost
  never fires. If you *shorten* lookahead below the attack time, or push a very
  hot signal into a very low ceiling, you can drive the smoother into the
  hard-clip path (`:119-120`) and hear distortion. Keep `attack_ms ≤ lookahead_ms`.

- **The hard clip is a backstop, not the main mechanism.** If you find it firing
  often, the *creative* stages upstream (MelLimiter, saturator) are overshooting
  hard — fix the input level, don't lean on the ceiling to do dynamics.

- **Latency reporting must stay in sync.** The plugin only ever calls
  `latency_samples()` once for the report (`axon_plugin.cpp:832`) and uses the
  same `reset()` to size the delay line, so they're guaranteed consistent. If you
  change `lookahead_ms`, you must re-`reset()` (which activation/sample-rate
  changes do) and re-report latency, or the DAW's delay compensation will drift.

- **4× is an *estimate*, not a proof.** ITU BS.1770 specifies a minimum of 4×
  for true-peak metering, and 4× catches the vast majority of inter-sample peaks
  to within ~0.05 dB. Pathological signals (sustained high-frequency tones near
  Nyquist) can have true peaks slightly above the 4× estimate. For an absolute
  guarantee you'd go 8× or 16×; Axon trades that last fraction of a dB for CPU.
  The hard-clip safety net covers anything the 4× detector underestimates.

- **Per-channel, independent gain.** Each channel runs its own limiter with its
  own gain state. For stereo material this means the two channels can momentarily
  receive slightly different gain reduction on asymmetric peaks, which can nudge
  the stereo image on extreme transients. (This is standard for a true-peak
  brickwall; link the channels if image stability ever matters more than the
  per-channel ceiling.)

- **It does not normalize loudness.** This is a peak ceiling, not a loudness
  target. Loudness targeting lives elsewhere (the MelLimiter / auto-gain), and
  the OUT meter reads *this* stage's output as the real master.

---

## 9. Where it sits in the Axon chain

```
audio → ort(autoeq controller) → SpectralMaskEq
      → RationalA (saturator)   → ort(ssl_comp) → BassMono
      → MelLimiter              → output trim  → TruePeakCeiling → output
                                                  ^^^^^^^^^^^^^^^
                                                  (this module — always last)
```

(Source chain comment: `axon_plugin.cpp:1-5`; the trim-then-ceiling final block:
`axon_plugin.cpp:1499-1511`.)

The True-Peak Ceiling is the terminal stage. Everything before it is tone and
dynamics shaping that the user can dial and (mostly) reorder; the ceiling is the
fixed guarantee at the very end.

### Related modules

- **MelLimiter** (`native/clap/src/`, StageID 5) — the *musical*, multi-band
  limiter that does the loudness-pushing work, with its own `MLC` ceiling param
  exposed to the host. It can overshoot true-peak; this module cleans up after
  it. Don't confuse its ceiling with this one.
- **RationalA saturator** & **SSL bus comp** — upstream creative stages whose
  output peaks this module bounds.
- **LoudnessMeter / OUT meter** — reads the ceiling's output as the real master
  (`axon_plugin.cpp:1499-1514`).
- **CompositeCeilingCfg** (`composite_meta.hpp:35-39`, parsed in
  `composite_meta.cpp:96-100`) — the bundle-driven configuration that feeds this
  module's `Config`.
