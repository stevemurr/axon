# True-Peak Ceiling: the always-last stage that makes -1 dBTP non-negotiable

> `native/clap/src/true_peak_ceiling.{hpp,cpp}` — a 4x-oversampled, lookahead,
> inter-sample peak limiter that runs dead-last in the Axon chain and guarantees
> the delivered master never exceeds a configured ceiling (shipped: **-1 dBTP**).
> All file:line references are to `origin/main`.

---

## A file that never touches 0 dBFS can still clip

Here is the trap this module exists for. Take a full-scale sine at exactly a
quarter of the sample rate, and sample it 45 degrees off its peaks. Every stored
sample reads ±0.7071 — a comfortable **-3 dBFS sample peak**. But the smooth
wave those samples describe still swings to ±1.0. The moment anything
*reconstructs* that wave — a DAC's output stage, an MP3/AAC/Opus codec's
synthesis filterbank, a phone's 44.1→48 kHz resampler — the signal is back at
full scale, 3 dB hotter than any number you ever stored. A file can pass every
sample-peak meter in existence and still crackle on somebody's earbuds.

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
  measure, and it *lies* — it ignores the bulges between samples.
- **True peak** (inter-sample peak): the largest value of the reconstructed
  analog wave, bulges included. Measured in **dBTP** — dB relative to full
  scale, true-peak. 0 dBTP means the reconstructed wave just touches full
  scale; positive dBTP means it overshoots (bad). The standard estimator (ITU-R
  BS.1770) is to **oversample**: synthesize extra samples between the real
  ones so the bulges become dots you can measure. Streaming platforms
  (Spotify, Apple Music, YouTube) normalize loudness and demand true-peak
  headroom for exactly the codec/SRC reasons above — Apple's Digital Masters
  guidance is a ceiling no higher than -1 dBTP. That convention is Axon's
  shipped default.

What makes Axon's answer interesting is *where the guarantee lives*. This chain
is full of learned components — an LSTM-driven auto-EQ, a TCN bus compressor, a
Mel-scale adaptive limiter with a water-filling gain solver. Any of them can
overshoot a peak target, individually or in combination, and the flashy
multiband limiter directly upstream **deliberately does not do true-peak
detection at all** (more on that division of labor in §7). The final word
instead belongs to 129 lines of dumb, deterministic C++: a windowed-sinc
polyphase upsampler, one one-pole smoother, a delay line, and a hard clip. It
is not a host knob, it is not in the reorderable stage list, it costs 0.58% of
process CPU, and — uniquely among Axon's interesting stages — it null-tests to
**exactly 0.0**. The last thing between a stack of neural networks and your
DAC is the most boring module in the repo. That is the point.

---

## 1. Architecture in one line

From the header (`true_peak_ceiling.hpp:4-6`):

```
4x polyphase FIR upsample -> peak detect -> lookahead delay -> gain smooth
   -> apply gain to delayed sample -> hard-clip safety net at the ceiling
```

As a signal-flow picture:

```
        in[i] ──► FIR history ──► 4x polyphase upsample ──► |max of 4| = peak_mag
          │                                                       │
          │                                       target = min(1, ceiling / peak_mag)
          │                                                       │
          │                                     one-pole smoother (attack / release)
          │                                                       │
          ▼                                                       ▼
   lookahead delay line ─────────────────► (x) ──► hard-clip [-ceil,+ceil] ──► out[i]
   (audio, delayed ~1.5 ms)               gain
```

Two independent guarantees stack:

1. **Transparent path** (does ~all the work): the gain control sees each peak
   a lookahead ahead of the audio, so reduction ramps in smoothly before the
   peak arrives. No clipping, no added distortion.
2. **Absolute path** (the actual contract): whatever the smoother missed, the
   final hard clip at ±`ceiling_lin_` makes `|out| <= ceiling` unconditional
   (`true_peak_ceiling.cpp:122-123`).

The class is pure C++ in namespace `nablafx`, with no CLAP or ONNX-Runtime
dependencies, so it unit-tests and benches standalone (`true_peak_ceiling.hpp:12`).

---

## 2. The oversampler: seeing between the samples

### Zero-stuffing, and the polyphase trick that deletes the zeros

To measure inter-sample peaks you reconstruct the in-between values. The
textbook recipe: insert 3 zeros after every input sample (that is the "4x",
`kOvsFactor = 4`, `true_peak_ceiling.hpp:44`), then lowpass at the *original*
Nyquist — the filter interpolates, filling the zeros with the values the
band-limited reconstruction would take there. 4x is the BS.1770-4 reference
rate for true-peak metering; it catches inter-sample peaks to within a few
hundredths of a dB on normal program material.

Filtering a zero-stuffed stream naively wastes 3 of every 4 multiplies on
zeros. The **polyphase decomposition** reorganizes the single 32-tap FIR
(`kFirTaps = 32`) into 4 sub-filters ("phases") of 8 taps each
(`kFirPhase = kFirTaps / kOvsFactor = 8`, `true_peak_ceiling.hpp:44-46`). Phase
`p` uses taps `p, p+4, p+8, …` and convolves them directly against the 8
most-recent *real* inputs — same math, no zeros, 4x less work. The inner loop
(`true_peak_ceiling.cpp:86-97`):

```cpp
double peak_mag = 0.0;
for (std::size_t p = 0; p < fac; ++p) {            // fac = 4 phases
    double acc = 0.0;
    for (std::size_t k = 0; k < ph; ++k) {         // ph = 8 taps/phase
        std::size_t tap = p + k * fac;             // 0..31
        // fir_hist_[(idx - 1 - k) mod ph] = (k+1)-th most-recent input
        std::size_t h_idx = (fir_hist_idx_ + ph - 1 - k) % ph;
        acc += fir_[tap] * fir_hist_[h_idx];
    }
    double mag = std::abs(acc);
    if (mag > peak_mag) peak_mag = mag;
}
```

Each phase output is one of the 4 oversampled values for this input sample;
`peak_mag` is the max of their magnitudes — the **true-peak estimate**. Because
only the max matters, the code never needs to know which phase corresponds to
which quarter-sample offset. Total cost: `4 x 8 = 32` multiply-accumulates per
sample per channel — one 32-tap FIR's worth, ~1.4 MMAC/s per channel at
44.1 kHz. The FIR history is a tiny 8-slot ring of the raw inputs
(`fir_hist_`, `true_peak_ceiling.hpp:60-63`); its index wrap `% ph` is a
compile-time power-of-two, which the compiler reduces to a mask
(`true_peak_ceiling.cpp:80`).

One subtlety worth knowing: with an even-length (32-tap) filter, each phase
branch has group delay `(15.5 - p)/4` input samples — never an integer. So *no*
phase passes the raw input samples through verbatim; all four outputs are
interpolated estimates of the waveform at four consecutive quarter-sample
offsets, roughly 3.1-3.9 samples in the past. The detector is therefore an
estimator even at the sample positions — which is fine, because the sample-
domain guarantee comes from the hard clip on the actual samples, not from the
detector.

### The filter design (`build_half_band_fir`, `true_peak_ceiling.cpp:15-42`)

The FIR is designed once, at `reset()` time, as a windowed sinc:

- **Cutoff** at the original Nyquist: in the oversampled domain's normalized
  units (Fs_ovs = 1, Nyquist = 0.5) that is `fc_norm = 0.5 / 4 = 0.125`
  (`true_peak_ceiling.cpp:20`). (The function name says "half-band" — in the
  strict multirate sense a half-band filter belongs to 2x interpolation; here
  the name is loose shorthand for "cut at half the *original* band." The design
  is a plain quarter-band lowpass at the 4x rate.)
- **Ideal sinc** evaluated over 32 taps centered at `(N-1)/2 = 15.5`
  (`true_peak_ceiling.cpp:23-27`), with the `k -> 0` limit handled explicitly
  (`2*fc_norm`).
- **Hann window** to tame truncation ringing (`true_peak_ceiling.cpp:29`).
- **Normalization**: taps are scaled so they sum to the oversample factor, 4
  (`true_peak_ceiling.cpp:33-41`). Standard polyphase convention — each of the
  4 phases then sums to ~1, i.e. unity DC gain *per phase*, so interpolated
  values land on the same amplitude scale as the inputs and comparisons
  against `ceiling_lin_` are meaningful.

---

## 3. The gain law, the lookahead, and a 3-tau derivation

### Peak → target gain

Given the 4x peak estimate, the desired gain is the pure peak-limiter law
(`true_peak_ceiling.cpp:105-108`):

```cpp
double target_gr = 1.0;
if (peak_mag > ceiling_lin_) {
    target_gr = ceiling_lin_ / peak_mag;   // exactly enough, no more
}
```

Below the ceiling: unity, bit-transparent (verified by test 2, §8). Above:
precisely the ratio that brings the offending peak down to the ceiling.
`ceiling_lin_ = 10^(dBTP/20)` (`true_peak_ceiling.cpp:60`) — 0.89125 at
-1 dBTP.

### Attack/release smoothing

Snapping the gain instantly would itself distort, so the gain is eased with a
one-pole smoother that picks between two speeds
(`true_peak_ceiling.cpp:110-111`):

```cpp
double coeff = (target_gr < gr_lin_) ? attack_coeff_ : release_coeff_;
gr_lin_ = coeff * gr_lin_ + (1.0 - coeff) * target_gr;
```

Attack (default **0.5 ms**) when clamping down, release (default **50 ms**)
when recovering. Coefficients are the usual `exp(-1/(tau*Fs))`
(`true_peak_ceiling.cpp:63-68`; at 44.1 kHz: attack 0.95566, release 0.99955),
with `ms` floored at 1e-3 so a zero config can't blow up the exponent. Note the
smoother's resting state is 1.0 — it converges *toward unity*, never toward
zero, so the gain path has no denormal-flush concerns.

### Lookahead: the delay line does the time travel

The clever part is that lookahead costs nothing extra — it *is* the delay line
(`true_peak_ceiling.cpp:99-104` explains the alignment in-source). At iteration
`i` the code:

1. computes `peak_mag` from the **current** input `x[i]`,
2. reads the audio sample from `lookahead_samples_` iterations **ago** out of
   the delay ring (`true_peak_ceiling.cpp:114-115`),
3. applies the freshly smoothed gain to that *old* sample.

So the gain control leads the audio by exactly the lookahead: by the time a
loud sample physically reaches the output, the smoother has had
`lookahead_samples_` samples of head start. Default lookahead is **1.5 ms**
(`true_peak_ceiling.hpp:26`), i.e. `round(1.5e-3 * Fs)` samples, clamped to a
minimum of 1 (`true_peak_ceiling.cpp:50-52`): 66 samples at 44.1 kHz, 72 at
48 kHz, 144 at 96 kHz.

**Why 1.5 ms against a 0.5 ms attack?** That ratio is the design. The lookahead
is 3 attack time-constants, so for an idealized step peak the smoother has
converged `1 - e^-3 ≈ 95%` of the way to the required reduction when the peak
arrives. Quantitatively: starting from unity gain, after time `t` the gain is
`g(t) = g_t + (1 - g_t) e^(-t/tau)`. At `t = 3*tau` the output peak is

```
P · g(3tau) = C + e^-3 · (P - C)      where P = peak, C = ceiling
```

For a peak 6 dB over the ceiling that residual is `0.0498 (P - C) ≈ 0.05 C` —
about **+0.42 dB** of would-be overshoot, which the hard clip shaves off. In
practice the clip almost never engages on program material; it exists so the
guarantee doesn't depend on the smoother at all. (Fine print: the detector's
own group delay, `(kFirTaps-1)/(2*kOvsFactor) ≈ 3.9` input samples, is not
compensated, so the *effective* lead is ~62 samples at 44.1 kHz rather than 66
— still 2.8 tau. See §9.)

### The hard clip is the contract

```cpp
double y = delayed * gr_lin_;
if (y > ceiling_lin_)  y = ceiling_lin_;      // true_peak_ceiling.cpp:121-123
if (y < -ceiling_lin_) y = -ceiling_lin_;
```

Everything above it is transparency engineering; these three lines are the
actual guarantee, and they are why the module's headline invariant —
`|out| <= ceiling`, always, for any input — is provable by inspection and
property-tested rather than merely tuned (§8).

---

## 4. Implementation notes: the boring module, done carefully

**Precision split.** The detector and gain path run in `double` (FIR taps,
history, accumulator, smoother state — `true_peak_ceiling.hpp:49-69`); the
audio delay line stays `float` (it stores what came in). Doubles cost nothing
here (32 scalar MACs) and buy bit-stability: the known-answer test holds to
1e-9 RMS across -O2/-O3 (§8).

**All allocation in `reset()`, none in `process()`.**
`reset(sample_rate)` (`true_peak_ceiling.cpp:46-69`) builds the FIR, sizes and
zeroes the delay ring (`delay_.assign(...)`, `:54`), zeroes the FIR history,
converts the ceiling to linear, resets the smoother to unity, and computes the
two pole coefficients. The hot path does arithmetic and ring indexing only —
no locks, no syscalls, no exceptions, no branches that allocate.

**In-place safe.** `process(in, out, n)` may be called with `in == out`
(`true_peak_ceiling.hpp:36-37`): iteration `i` reads `in[i]` before writing
`out[i]` and never revisits earlier inputs (history lives in its own ring), so
aliasing is harmless. The plugin uses exactly this property — it feeds `work_l`
/`work_r` in and writes the channel's `out_buf` (§5).

**The two ring wraps are deliberately different.** The 8-slot FIR history wraps
with `% ph` — unsigned, constexpr power-of-two, compiled to a mask
(`true_peak_ceiling.cpp:80`). The lookahead ring's length is a *runtime,
non-power-of-two* value (66/72/144…), so a `%` there is a real integer
division per sample. Since the index only ever advances by 1, compare-and-reset
is exactly equivalent (`true_peak_ceiling.cpp:116-119`):

```cpp
if (++delay_idx_ == delay_.size()) delay_idx_ = 0;
```

That branch-wrap is the one perf change this module has ever needed — see §6
for the measured story.

**Latency is self-reported and self-consistent.**
`latency_samples()` returns `lookahead_samples_` (`true_peak_ceiling.hpp:40`)
— the same variable that sized the delay ring in the same `reset()` call, so
the report can't drift from the actual delay.

---

## 5. Wiring: dead-last, per-channel, and not a knob

All anchors in `native/clap/src/axon_plugin.cpp` unless noted.

### Outside the reorderable stage list — on purpose

Axon's seven creative stages (BassMono, SSL EQ, AutoEQ, Reverb, Widener,
SslComp, MelLimiter) live in `processor_order` and are freely reorderable from
the UI (`StageID` enum at `:106-117`, `kDefaultStageOrder` at `:125`). The
TruePeakCeiling has **no StageID**. It runs after the stage loop ends, fused
with the output trim (`:2111-2126`):

```cpp
// Trim + TruePeakCeiling — always last, not user-reorderable. This is the
// REAL master; the OUT meter reads it (so driving the limiter shows the
// actual target loudness), and Auto Gain is applied *after* metering.
if (amt.trim_lin != 1.f) { /* trim work_l / work_r */ }           // :2117-2120
for (uint32_t ch=0;ch<n_ch;++ch) {
    float* blk=(ch==0)?work_l:work_r;
    plug.chains[ch].ceiling.process(blk,plug.chains[ch].out_buf.data(),kBlockSize);
    ...
}
```

Order matters twice here:

- **Trim runs *before* the ceiling** (`:2117` vs `:2121`), so even the user's
  output trim cannot push the master past the ceiling. (The chain sketch in
  the file-header comment at `:5` says "TruePeakCeiling → output trim" — that
  comment is stale; the code order is trim-then-ceiling, and it's the safe
  one.)
- **The OUT meter reads the ceiling's output** (`:2132-2134`) — the delivered
  master — and the monitoring-only Auto Gain trim is applied after metering
  (`:2153-2172`), so pushing the limiter's Drive shows the true target
  loudness on the meter regardless of level-matched monitoring.

Each channel owns its own instance — `TruePeakCeiling ceiling;` in
`ChannelChain` (`:649`, per-channel note at `:596`) — with independent gain
state (implications in §9).

### Configured by the bundle, not the host

The ceiling is **not** host automation. There is no CLAP param for it; the two
host-facing composite knobs are AMT and TRM (plus the stage knobs). Its four
numbers travel a fixed pipeline, and the defaults agree at every hop
(-1.0 dBTP / 1.5 ms / 0.5 ms / 50 ms):

| Hop | Where |
|---|---|
| Python exporter writes it | `axon/export/composite.py:220-221` (field declared `:78`; every bundle build re-composes this) |
| Bundle JSON carries it | `weights/axon_bundle/axon_meta.json:577-582` (`"ceiling"` object) |
| C++ parses it | `composite_meta.cpp:96-101` → `CompositeCeilingCfg` (`composite_meta.hpp:35-40`). `ceiling_dbtp` is **required** (`.at()`); the three time params fall back to defaults (`.value()`), and a negative `lookahead_ms` is rejected at load (`:99`) |
| Plugin applies it per channel at activate | `axon_plugin.cpp:1211-1218` — constructs `TruePeakCeiling::Config` from `g_state->axon_meta.ceiling`, then `ch.ceiling.reset(sample_rate)` |

The module's own `Config` defaults (`true_peak_ceiling.hpp:24-29`) match the
bundle, so a standalone-constructed instance (tests, benches) behaves like the
shipped one. The CLAP `reset()` callback also re-resets each channel's ceiling
(`axon_plugin.cpp:1283, 1296`), flushing the delay line and smoother with all
other stage state.

### Latency accounting

`compute_latency_` (`:941-945`) reports to the DAW:

```cpp
uint32_t lat = kBlockSize;                                        // 128-sample accumulator
lat += static_cast<uint32_t>(plug.chains[0].ceiling.latency_samples());
```

The ceiling's lookahead is one of only two *unconditional* latency sources
(source-comment inventory at `:936-940`; file header `:15`): the 128-sample
block accumulator plus the lookahead, with spectral-EQ / SSL-comp latency
added only when those stages are active. Total floor at 44.1 kHz:
128 + 66 = **194 samples ≈ 4.4 ms**.

| Sample rate | Lookahead samples | Lookahead ms |
|---|---|---|
| 44.1 kHz | round(66.15) = 66 | ~1.50 |
| 48 kHz | 72 | 1.50 |
| 96 kHz | 144 | 1.50 |

If you ever make `lookahead_ms` dynamic, the invariant to preserve is:
re-`reset()` and re-report latency together (activation and sample-rate changes
already do), or the DAW's plugin-delay compensation drifts.

---

## 6. Measured stories

### June 2026 DSP pass: one idiv, bit-identical, and a test debt paid

Module 8 of the per-module perf/quality pass (commit `ff12112`, 2026-06-06).
Profiling found the only real per-sample division was
`delay_idx_ = (delay_idx_ + 1) % delay_.size()` — a runtime non-power-of-two
modulus (the FIR-history wrap was already a compile-time mask). The index
advances by 1, so compare-and-reset is exactly equivalent; the branch-wrap now
in `true_peak_ceiling.cpp:116-119` shipped with:

- **Null test:** three lookahead configs, `max|new - old| = 0.000e+00` —
  **bit-identical**. This module is the well-conditioned end of the Axon
  spectrum: a scalar feedforward FIR plus one smoother, no FFT whose rounding
  is buffer-alignment-sensitive, no sort-based discontinuous solver. Contrast
  the MelLimiter and Auto-EQ, where even *no-change* rebuilds differ at
  -75..-90 dBFS in the warmup transient and the strict -120 dBFS null bar is
  unreachable ([the Mel Limiter dive](mel-limiter.md), §7). Here the
  strictest possible bar — zero — just passes.
- **Throughput:** ~9 ns/sample/ch standalone, ~0.09% of a 128-block budget in
  stereo. The honest note in the commit: the idiv removal is within
  measurement noise at this size — it shipped as a consistency micro-opt (the
  same change was *rejected* for the MelLimiter, where it couldn't be
  null-proven), not a hot-spot fix.
- **Test debt:** until then TruePeakCeiling had *no test in the CMake suite*
  (only the un-built `tests/test_dsp.cpp` referenced it). The pass added
  `tests/test_true_peak.cpp` (§8) and `bench/bench_true_peak.cpp`, precisely so
  the wrap change (and any future refactor) is pinned by a known-answer
  baseline.

### July 2026 stage ranking: explicitly leave-alone

The instrumented per-stage CPU ranking
(`native/clap/docs/perf_stage_ranking.md`, 2026-07-05, buf=128, all-stages
scenario) measures the fused **TrimCeiling** slot — trim plus *both* channels'
ceilings, recorded once per block (`AXON_ST_TRIM_CEILING`,
`axon_plugin.cpp:2128`) — at:

| Share of process() | mean/block | p95 | max |
|---|---|---|---|
| **0.581%** (0.575% at buf=512) | **2.62 µs** | 4.01 µs | 44.63 µs |

Cross-check: 2.62 µs over 128 samples x 2 channels ≈ **10.2 ns/sample/ch**,
matching the standalone bench — the stage costs in-chain what it costs on the
bench, no cache surprises. The ranking's §5 verdict lists TrimCeiling among the
explicitly-cheap stages: *leave alone; no proposal clears any reasonable ROI
bar*. For scale: the two ONNX stages above it are 58.5% and 33.5%. The entire
non-negotiable delivery guarantee costs less than the level meters would
tolerate doubling.

`bench/bench_true_peak.cpp` reproduces the standalone number: 1 s of ±1.2
uniform noise at 48 kHz through block sizes 64/128/512, reported as
ns/sample/ch and as stereo percent of the 128-block real-time budget
(`c++ -O3 -std=c++17 -I src bench/bench_true_peak.cpp src/true_peak_ceiling.cpp`).

---

## 7. The two ceilings: why the flashy limiter upstream doesn't do this job

The classic user confusion: Axon has a knob named **Ceiling** (the
MelLimiter's `MLC`) *and* a stage named True-Peak Ceiling. They are different
instruments with different contracts:

| | MelLimiter `MLC` | TruePeakCeiling |
|---|---|---|
| What it is | The *musical* clamp inside the 26-band Mel-scale loudness maximizer | The *delivery* guarantee, last in chain |
| Detector | **Sample peak** (its brickwall does no oversampling — deliberate) | **True peak**, 4x oversampled per BS.1770 |
| Units / range | dBFS, host knob, -12..0, default **0.0** | dBTP, fixed by bundle meta, **-1.0** |
| Who sets it | The user (`axon_plugin.cpp:1454` converts dBFS→linear per block) | The bundle (`axon_meta.json:577-582`), no host param |
| Scope | Only the limiter's wet path; subject to wet/dry (`MLI`) | Everything, including trim; cannot be bypassed or reordered |

The MelLimiter's own brickwall carries a "true-peak" comment in its source,
but the June perf findings pinned down the truth: **it is a sample-peak
limiter — no oversampled detection — and that is correct by architecture**,
because inter-sample peaks are handled downstream by this stage
([the Mel Limiter dive](mel-limiter.md), §7). The division of
labor is tidy:

- the MelLimiter does the *loudness* work — per-band water-filling gain
  solve, drive, adaptive attack — and with `MLC` at its 0.0 dBFS default it
  slams samples right up to full scale;
- the TruePeakCeiling does the *delivery* work — it takes that hot,
  sample-legal signal and makes the reconstructed waveform respect -1 dBTP,
  without a filterbank, a model, or an opinion.

Running true-peak detection inside the MelLimiter would duplicate the
oversampler per band-limiter instance for zero benefit — any true-peak excess
it left behind is caught 66 samples later anyway. Conversely the
TruePeakCeiling does no loudness targeting whatsoever: it only ever
attenuates, adds no makeup gain, and its released state is unity. Loudness
belongs to the MelLimiter and the (monitoring-only) Auto Gain.

---

## 8. Tests: a property, a transparency check, and a KAT

`tests/test_true_peak.cpp` (standalone, no CLAP/ORT; also in the CMake suite):

1. **Ceiling never exceeded** (`:33-58`). One second of a deliberately illegal
   signal — 1.4 amplitude 220 Hz + 0.6 x 7 kHz, plus +2.0 spikes every 4096
   samples — through the default config. Asserts every output sample is finite
   and `peak <= 10^(-1/20) + 1e-6` (`:52`). This is the hard-clip contract
   exercised as a property.
2. **Latency and transparency** (`:60-79`). Asserts
   `latency_samples() == round(1.5e-3 * 44100)` (`:65`), then feeds a 0.3
   amplitude sine (well under the ceiling) and asserts the output equals the
   input delayed by exactly that many samples to <1e-6 (`:77`) — below the
   ceiling this stage is a pure delay.
3. **Known-answer regression** (`:84-107`). A fixed two-tone input (1.3 x 997 Hz
   + 0.4 x 5 kHz, i.e. hot enough to engage limiting); asserts RMS to 1e-9,
   peak to 1e-7, and four spot samples to 1e-5 against locked references
   (`:94-105`), bit-stable across -O2/-O3. A nice detail: the reference peak
   `8.912509083748e-01` sits ~3e-8 *below* `10^(-1/20) = 0.891250938…` — the
   baseline signal genuinely grazes the ceiling, so the KAT pins the clip and
   smoother paths, not just the pass-through. This is the baseline that
   guards the branch-wrap (and any future refactor) against silent drift.

---

## 9. Honest limitations and open edges

- **4x is an estimate, not a proof of the analog waveform.** BS.1770 mandates
  >=4x for metering, and 4x catches typical program material to within a few
  hundredths of a dB, but pathological content (sustained tones near Nyquist,
  adversarial phase alignments) can reconstruct slightly above the 4x
  estimate. An 8x/16x detector would tighten the bound at proportional cost.
  Axon's stance: the hard clip guarantees the *sample*-domain ceiling
  absolutely, and the 4x detector keeps the *reconstruction* within a hair of
  it; the last fraction of a dB is traded for CPU. If a distributor's meter
  (also 4x, per spec) is the acceptance test, the estimator and the judge
  measure the same thing.
- **The hard clip is distortion when it fires.** The transparent path covers
  ~95% of a step overshoot (§3); what remains is clipped, which is audible in
  principle. If the clip engages often, the fix is upstream gain staging (the
  MelLimiter's `MLC`/Drive), not leaning on the backstop. Corollary: keep
  `attack_ms <= lookahead_ms` if you ever retune — shrinking lookahead below
  ~2-3 attack taus shifts work from the smoother to the clip.
- **Uncompensated detector group delay.** The peak estimate lags the input by
  `(kFirTaps-1)/(2*kOvsFactor) ≈ 3.9` samples, so the effective lead is
  ~62/66 samples at 44.1 kHz — 2.8 tau instead of 3.0. Harmless at current
  constants (the e^-2.8 residual is 6%, still clipped), but worth knowing if
  lookahead is ever shortened: the margin erodes faster than the config
  number suggests.
- **Per-channel, unlinked gain.** Each channel smooths its own gain
  (`axon_plugin.cpp:596, 649`), so a hard one-sided transient momentarily
  attenuates one channel more than the other, nudging the stereo image. This
  is standard for a final true-peak brickwall (and the MelLimiter upstream
  *is* stereo-linked for the musical limiting); link the ceilings' gains if
  image stability under extreme asymmetric transients ever outranks
  per-channel headroom.
- **Auto Gain runs after the ceiling — and can exceed it at the monitor.**
  Auto Gain is a monitoring-only, level-matched-bypass trim applied to
  `out_buf` *after* the OUT meter (`axon_plugin.cpp:2153-2172`, clamped
  ±24 dB). If the chain makes the program *quieter* than the input, Auto Gain
  boosts, and the monitored signal can pass -1 dBTP. This is by design — the
  printed master must be rendered with Auto Gain off (its doc and the source
  comment both say so) — but it means the "non-negotiable" claim is about the
  render path, not the monitor path.
- **Stale comments, not stale code.** The header's framing
  (`true_peak_ceiling.hpp:8-10`, "backstop for the TONE chain — the LA-2A comp
  and saturator can overshoot") predates the current chain: the LA-2A is long
  removed, the saturator is dormant (kept but not in `processor_order`,
  `axon_plugin.cpp:94-99`), and the overshoot sources today are the SSL bus
  comp, the MelLimiter, and the trim. Likewise the file-header chain sketch at
  `axon_plugin.cpp:5` shows "TruePeakCeiling → output trim" while the code
  runs trim first (`:2117-2126`). Both are comment drift only — the *rationale*
  (deterministic backstop behind learned/nonlinear stages, ceiling
  non-negotiable without coloring dynamics) is exactly as true of the 2026
  chain as of the one it was written for.
- **`build_half_band_fir` is loosely named.** It designs a quarter-band
  (fc = Fs_ovs/8) windowed-sinc interpolation lowpass, not a half-band filter
  in the strict multirate sense (§2). Rename candidate if it ever confuses.
- **Config is fixed at activate.** Changing the bundle's ceiling values
  requires re-activation; there is deliberately no live automation of the
  ceiling (a moving delivery ceiling is a mastering anti-feature, and a moving
  lookahead would need a latency re-report, see §5).
- **Perf: closed.** 0.581% of process CPU, branch-wrap shipped bit-identical,
  ranked leave-alone. The only conceivable micro-win left (vectorizing the
  32-MAC loop) is noise at this share; the well-conditioned null (exactly 0.0)
  means such a change *could* be proven safe if anyone ever cared — this is
  the one stage in the chain where the strictest validation bar is actually
  available.

---

*Supersedes `docs/true_peak_ceiling.md` (2026-06-05 tutorial): all of its
theory, wiring, parameter, and gotcha content is carried forward here, with
drifted line anchors re-verified against origin/main (the old doc's
`axon_plugin.cpp` anchors predate the SSL-EQ/reverb/widener additions, and
`axon_meta.json`'s ceiling block moved from :262 to :577), the branch-wrap
delay-index change incorporated, the stale LA-2A framing flagged, and the
measured June/July numbers added. The MelLimiter sample-peak note is imported
from `native/clap/docs/mel_limiter_perf_findings.md`, whose remaining findings
now live in [the Mel Limiter dive](mel-limiter.md).*
