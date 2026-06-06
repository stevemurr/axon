# Suppressing Frequency Buildups — DSP Approaches, Tradeoffs, and What Fits Axon

**Status:** research report. No code changes. Goal: survey the DSP techniques for
taming unpleasant frequency buildups (resonances, harshness, mud, ringing), look
hard at the referenced spectral-compressor codebase, and recommend what is worth
prototyping inside Axon given its existing STFT infrastructure.

---

## 1. What "frequency buildup" actually means

"Buildup" is an umbrella term for several distinct problems, and the right tool
depends on which one you have:

| Symptom | Physical cause | Time character |
|---|---|---|
| **Resonance / ringing** | A narrow frequency that is over-emphasized by a room mode, mic, instrument body, or a high-Q filter somewhere upstream. | Often **sustained / decaying** — it rings after the transient. |
| **Harshness** | A broad-ish region (typically 2–5 kHz "presence", 6–10 kHz "sibilance/air") that becomes fatiguing when dense material stacks up. | **Dynamic** — only objectionable on loud/dense passages. |
| **Mud** | Low-mid (200–500 Hz) energy summing across many tracks. | **Program-dependent** buildup. |
| **Masking buildup** | Many sources occupying the same band, so nothing is individually "wrong" but the sum is congested. | Statistical, over time. |

Key insight that drives everything below: **the offending energy is defined
*relative to its neighbours and to time*, not by an absolute level.** A 3 kHz peak
that is 8 dB above the surrounding spectrum is a resonance; the same 3 kHz at the
same absolute level sitting *flush* with its neighbours is just tone. This is why
static EQ is a blunt instrument and why every "smart" approach is some form of
*detect-what-sticks-out → attenuate-only-that → only-when-it-sticks-out*.

The two axes that organise the whole design space:

- **Frequency selectivity** — broadband → few bands → many bands → per-FFT-bin.
- **Detection reference** — absolute threshold → smoothed spectral envelope
  (relative) → time envelope (dynamic) → perceptual/masking model → learned.

---

## 2. The landscape at a glance

| Family | Resolution | Detection | Latency | Artifacts | CPU | Control |
|---|---|---|---|---|---|---|
| Static notch / parametric EQ | 1 band, set Q | manual | ~0 (IIR) | phase smear, always-on | trivial | manual, surgical |
| Dynamic EQ | few bands | per-band threshold (time) | ~0 (IIR) | low; transient interaction | low | precise, manual |
| Multiband compressor | 3–6 bands | per-band threshold (time) | low (xover) | pumping, smearing | low | coarse |
| **Resonance suppressor (Soothe-style)** | many narrow, **adaptive** | peak-above-smoothed-envelope (relative + time) | mid (FFT) | musical noise if pushed | mid–high | semi-auto |
| **Spectral compressor (per-bin)** | every FFT bin | per-bin threshold (time), threshold *shape* | mid (FFT) | pre-echo, smearing, musical noise | high | global + curve |
| Spectral subtraction / gating | every bin | bin vs noise/reference floor | mid (FFT) | musical noise (classic) | mid | reference-driven |
| Cepstral / LPC whitening | envelope vs fine structure | model split | low–mid | over-flattening | mid | macro |
| Perceptual / masking-model | critical bands | psychoacoustic (sharpness/roughness) | mid | naturalness if mis-tuned | high | macro |
| Modal / decay-based (de-ring) | resonant modes | by **decay time**, not level | mid–high | complex | high | niche |
| Learned / neural | model-defined | trained on examples | model-dependent | training-dependent | high | macro |

The rest of the document explains each, then zooms into the spectral compressor
codebase, then recommends.

---

## 3. Resonant-peak (filter-domain) approaches

These operate with biquad-style filters and a time-domain detector. Low latency,
phase coloration, very mature.

### 3.1 Static notch / parametric cut
The baseline. A peaking filter at fixed frequency/gain/Q. **Use when** the
resonance is constant (a fixed room mode, a known mic peak). **Downside:** it is
always cutting, even when the resonance isn't ringing, which dulls the rest of the
program. Surgical and cheap; not adaptive.

> Sweep-to-find trick: boost a high-Q bell +12 dB and sweep until the resonance
> screams, then invert the gain. Good for *finding* the frequency even if you end
> up using a smarter tool to *tame* it.

### 3.2 Dynamic EQ
A peaking filter whose gain is driven by a per-band envelope follower: it only
cuts when energy in that band crosses a threshold, then releases. **This is the
single best "manual but musical" tool** — TDR Nova, Pro-Q dynamic bands, etc.

- **Pros:** near-zero latency (IIR), transparent, you control exactly one
  problem, no FFT artifacts.
- **Cons:** you must *find and set* each band by hand; a handful of bands won't
  chase a resonance that drifts in frequency; high-Q dynamic bells still smear
  transients a little.
- **Detection reference:** absolute per-band threshold (optionally relative if the
  plugin offers it).

### 3.3 Multiband compressor
Split into 3–6 bands with crossovers, compress each. Coarse cousin of dynamic EQ.
Good for broad "tame the highs when it gets loud" moves; too blunt for narrow
resonances and the crossovers introduce phase/transient artifacts. Axon already
ships a **neural SSL-style compressor** and a **Mel Limiter**, so broadband/multi
dynamics are largely covered.

### 3.4 Adaptive resonance suppressor — the "Soothe" class
This is the filter-domain idea taken to its logical end and made automatic. The
defining trait (per oeksound's own description and third-party teardown): it does
**not** rely on a fixed bank of filters at fixed frequencies. Instead, each frame
it:

1. Analyses the spectrum at high resolution.
2. Builds a **smoothed reference envelope** (what the spectrum "should" look like
   locally — a moving average across frequency).
3. Finds **peaks that stick out above that envelope** (local maxima, with an
   estimated bandwidth/Q).
4. Applies **dynamic, narrow attenuation only to those peaks, only while they
   exceed the reference**, tracking them as they move in frequency.

So the detection reference is *relative* (peak-vs-local-average) **and** *dynamic*
(time envelope). That combination is why it feels transparent: it ignores energy
that is flush with its neighbours and ignores peaks that aren't currently ringing.

- **Pros:** automatic, follows drifting resonances, surgical, broadband coverage,
  "set depth and forget".
- **Cons:** higher latency (it's effectively FFT-driven), can introduce a soft
  "lisp"/musical-noise character or dull the top end if pushed hard; the magic is
  in the peak-picking + smoothing heuristics, which are non-trivial to get clean.

This class can be implemented either as a *bank of many dynamic bells* steered by
the peak detector, or directly in the **FFT domain** (next section) — the line
between "resonance suppressor" and "spectral compressor with a smoothed threshold
curve" is genuinely blurry, which is the central finding of this report.

---

## 4. Spectral (STFT) approaches

These move into the frequency domain: window → FFT → modify per-bin magnitudes →
IFFT → overlap-add (OLA). They give arbitrarily fine frequency selectivity at the
cost of latency (= window size) and a family of FFT-specific artifacts.

### 4.1 Per-bin spectral compression — the referenced codebase

The repo `polarity/plugin.nih.polarity-sc-dark` is a re-skinned build of **Robbert
van der Helm's Spectral Compressor** (the `nih-plug` example; GPL-3.0, the source
header still reads *"Spectral Compressor: an FFT based compressor … 2021-2024
Robbert van der Helm"*). The DSP lives in
`plugin/src/compressor_bank.rs` (~2,150 lines). It is the cleanest open reference
for this technique that exists. Here is exactly what it does.

**One compressor per FFT bin.** `CompressorBank` holds struct-of-arrays sized to
`MAX_WINDOW_SIZE/2 + 1` bins: per-bin thresholds (up & down), ratios, soft-knee
coefficients, and a per-bin **envelope follower**.

**Envelope follower (per bin), `update_envelopes()`** — a one-pole peak follower
run at the STFT *frame* rate, on the bin magnitude `bin.norm()`:

```rust
// effective_sample_rate = sample_rate / (window_size / overlap_times)
let attack_old_t  = (-1.0 / (attack_ms/1000.0 * effective_sample_rate)).exp();
let release_old_t = (-1.0 / (release_ms/1000.0 * effective_sample_rate)).exp();
// per bin:
if *envelope > magnitude {  // release
    *envelope = release_old_t * *envelope + (1.0-release_old_t) * magnitude;
} else {                    // attack
    *envelope = attack_old_t  * *envelope + (1.0-attack_old_t)  * magnitude;
}
```
Note the `effective_sample_rate` correction: with a 2048 window at 4× overlap the
function runs once per 512 samples, so the time constants are scaled accordingly.

**Transfer curve (per bin), `compress()` → `compress_downwards`/`compress_upwards`**
— the envelope, in dB, is run through a standard hard/soft-knee compressor curve.
The soft knee is the parabolic interpolation from **Giannoulis et al., *Digital
Dynamic Range Compressor Design*** (the same paper most modern compressors cite):

```rust
fn compress_downwards(input_db, threshold_db, ratio, knee_width_db, a, b) -> f32 {
    let knee_start = threshold_db - knee_width_db/2.0;
    let knee_end   = threshold_db + knee_width_db/2.0;
    if input_db <= knee_start { input_db }                       // below: untouched
    else if input_db <= knee_end {                              // in knee: parabola
        let x = input_db + b; input_db + a*x*x
    } else { threshold_db + (input_db - threshold_db)/ratio }    // above: ratio
}
```
`compress_upwards` is the mirror image (boosts bins that fall *below* a threshold).
The plugin runs **both directions simultaneously**, so the net per-bin gain is:

```rust
raw_gain_difference_db[bin] = downwards_out + upwards_out - 2*envelope_db;
```
i.e. "where should this bin be" minus "where it is", applied as a magnitude scale.

**Threshold *shaping* — the part that makes it a buildup-suppressor.** The per-bin
thresholds aren't flat. They're built from a global threshold **plus a configurable
spectral slope** (`PINK_NOISE_SLOPE_OFFSET_DB_PER_OCT = 3.0` → a pink reference)
**plus an editable threshold curve** (`curve.rs`, multi-point) **or a sidechain /
match curve**. So you can tell it "the spectrum *should* look pink (or like this
curve, or like this other track)" and downward compression then **ducks whatever
pokes out above that target shape** — which is precisely *frequency-buildup
suppression*. The "match" modes (`match_curve.rs`, `match_level.rs`) and
pink-noise mode are exactly this.

**Cross-bin gain smoothing — the artifact tamer, `smooth_gain_differences()`.**
Independent per-bin gains create the classic "musical noise"/warbling. The code
fixes this by smoothing the *gain-reduction curve* across frequency with a
**log-frequency moving average** (prefix-sum sliding window), radius up to
`GAIN_SMOOTHING_MAX_RADIUS_LN = ln 2` (one octave):

```rust
let radius_ln = smoothing_amount * GAIN_SMOOTHING_MAX_RADIUS_LN; // up to 1 octave
// sliding [left_idx, right_idx] in ln(freq); averaged = sum/count via prefix sums
```
This is the single most important trick for making per-bin processing sound smooth
— **borrow this regardless of which approach you pick.**

**Other features:** STFT with windowed OLA (window size & overlap selectable —
this sets both frequency resolution and latency), **freeze** (hold the current
spectral gains), **sidechain spectral matching** (`process_sidechain`,
`compress_sidechain_match`), and **IR export** of the resulting filter.

**Verdict on the codebase:** genuinely useful as a reference. It is GPL-3.0, so
you can't lift the code into a non-GPL product, but the *algorithm* is standard and
well-commented. The three ideas worth taking: (1) per-bin dual compressor with a
Giannoulis soft knee, (2) **threshold shaped to a reference spectral curve** =
buildup suppression, (3) **log-frequency gain smoothing** to kill musical noise.

### 4.2 Spectral "duck what exceeds the smoothed envelope" (Soothe in FFT domain)
A lighter variant of 4.1 aimed specifically at resonances. Instead of a per-bin
threshold *curve*, derive the reference **from the signal itself** each frame:
smooth the current magnitude spectrum (e.g. 1/3-octave or a cepstral lifter) to get
the "envelope it ought to have", then attenuate bins by how far they exceed it,
with a per-bin time release. This is essentially Soothe's core expressed in bins,
and it needs no manual curve. Add the log-frequency gain smoothing from 4.1 and a
depth/sharpness control and you have a credible resonance suppressor.

### 4.3 Spectral subtraction / spectral gating
Estimate a (noise/reference) floor per bin and subtract it / gate below it. The
canonical **de-noise / de-hum / de-ring** approach. For *resonance/ring* control
the useful framing is "a resonance is energy that persists after the transient",
so a per-bin *downward expander/gate* with a slow-ish release shortens ringing
tails (de-reverb-like). Famous for musical noise when aggressive — same smoothing
remedies apply.

---

## 5. Other approaches worth bubbling up

These weren't in the prompt but are relevant:

- **Cepstral / LPC whitening (spectral envelope vs fine structure).** Use a
  cepstrum (or linear prediction) to separate the *coarse spectral envelope* from
  the fine structure, then partially flatten the envelope toward a target tilt.
  This is a macro "de-color / de-resonate" move and is cheap-ish. Risk:
  over-flattening removes desirable character. Conceptually it's what 4.2 does, but
  parameterised by an envelope model instead of a smoothing kernel.

- **Perceptual / masking-model driven.** Drive the detector with a psychoacoustic
  model (Bark/ERB critical bands, Zwicker **sharpness**, **roughness**, partial
  loudness/masking) instead of raw dB. You then suppress energy that is
  *perceptually* harsh, which can be gentler than dB-based detection for the same
  subjective result. Higher complexity; tuning naturalness is the hard part. Good
  fit for a "harshness" control specifically (2–8 kHz).

- **Modal / decay-based de-ringing.** Detect resonances by their **decay time**
  (a ringing mode has a long, narrow tail), not their level — e.g. track per-bin
  reverberation/decay and suppress only the sustained tonal partials. This is the
  most *targeted* anti-resonance idea (it leaves transient energy alone entirely)
  but also the most complex; overlaps with dereverb research.

- **All-pass / phase-decorrelation.** Rotating phase around a resonance can reduce
  its perceived strength without level cuts. Niche, mostly useful in synthesis;
  unlikely to be worth it here.

- **Learned / neural suppression.** Train a small model to emit per-band (mel) or
  per-bin gain reductions from examples of "harsh in → tamed out". **This is the
  approach most aligned with Axon's identity** — Axon is already a neural mastering
  chain with an STFT mel-band controller (the Auto EQ) and an ONNX runtime. A
  resonance suppressor could be a *new control head* on the existing controller,
  or a DSP suppressor whose depth is modulated by a learned estimator.

---

## 6. Tradeoff deep-dive (the things that actually bite)

- **Latency.** Filter-domain (dynamic EQ, suppressor-as-bells) ≈ 0. Any STFT
  approach costs **one window** of latency. Axon's `SpectralMaskEQ` already runs
  `n_fft = 4096, hop = 2048` (~93 ms @ 44.1k) and the Mel Limiter adds more, so an
  STFT suppressor that *reuses that window* is nearly free in added latency; a
  *new, larger* FFT is not.

- **Frequency vs time resolution (the fundamental STFT tradeoff).** Big window =
  fine frequency bins (good for low-frequency resonances) but smeared transients
  and **pre-echo** (a cut "leaks" backwards in time before the transient). Small
  window = punchy but coarse in frequency and grainy. 2048–4096 @ 44.1k with 4×
  overlap is the usual compromise; resonance work often wants the *larger* end
  because resonances are narrow.

- **Musical noise / warbling.** The #1 failure mode of per-bin processing.
  Independent rapidly-changing bin gains sound like swirling artifacts. Mitigations
  (all present or implied in the polarity code): **cross-bin gain smoothing in
  log-frequency**, per-bin time smoothing (attack/release), higher overlap, and not
  pushing depth too far.

- **Transparency vs correction.** Relative+dynamic detection (suppressor / spectral
  comp with shaped threshold) is the most transparent because it ignores
  flush-with-neighbours and not-currently-ringing energy. Absolute static EQ is the
  least.

- **CPU.** Per-bin (thousands of compressors + smoothing) is the heaviest;
  few-band dynamic EQ is cheap. Reusing an existing FFT amortises the transform
  cost, leaving just the per-bin arithmetic.

- **Control surface.** Dynamic EQ = precise but manual. Suppressor/spectral comp =
  one or two macros (depth, sharpness, tilt/curve) but less surgical. Decide which
  the Axon UX wants.

---

## 7. Recommendation for Axon

Axon is unusually well-positioned: it already has a **windowed STFT magnitude-mask
engine** (`spectral_mask_eq.hpp`: `n_fft=4096`, `hop=2048`, 64 mel bands, per-band
gain mask via OLA), a **Mel Limiter** (STFT), and a **per-stage spectrum analyzer**.
A buildup suppressor is mostly *new control logic on top of machinery that exists*.

Three options, in increasing ambition:

**Option A — "Resonance Tamer" reusing the SpectralMaskEQ engine (recommended
first step).** Per frame: take the existing FFT magnitudes, build a smoothed
reference envelope (log-frequency moving average, exactly the polarity trick), and
compute a per-band (or per-bin) downward attenuation = `min(0, ref - mag)` scaled
by a **Depth** control, with per-band attack/release. Apply it as an extra mask on
top of the Auto-EQ mask, then OLA. **Why:** near-zero added latency (shares the
4096 window), tiny CPU, no new FFT, and it directly implements the
"duck-what-sticks-out" definition of a resonance. Controls: Depth, Sharpness
(smoothing radius), and a frequency Range. This is a Soothe-lite that costs almost
nothing because the transform is already paid for.

**Option B — Full per-bin spectral compressor (port the algorithm, not the code).**
Implement `compress_downwards`/`upwards` + per-bin envelope + **log-frequency gain
smoothing** + a **threshold curve shaped to a reference** (flat / pink / matched).
This gives both buildup suppression (downward, shaped threshold) and "fill the
dips" (upward) and is a more general tool than A. Heavier CPU and a bigger UI.
Re-derive from the Giannoulis paper + this report so you stay clear of the GPL
source.

**Option C — Neural resonance head (most on-brand, most work).** Add a control
head to the existing STFT controller that emits per-mel-band suppression gains,
trained on harsh→tamed pairs (or distilled from a DSP suppressor like Option A).
Inference already exists (ONNX runtime); this would make "de-harsh" part of the
learned chain rather than a bolt-on. Highest payoff, highest cost.

**Suggested path:** ship **Option A** as a DSP module first (fast, low-risk,
reuses the FFT, immediately useful), keep **Option B**'s threshold-curve idea as
the upgrade for a dedicated spectral-dynamics stage, and treat **Option C** as the
research track that fits Axon's neural identity. In all three, **steal the
log-frequency gain smoothing** — it's the cheap secret to making spectral
suppression sound smooth.

What to *not* build: another broadband/multiband compressor — Axon's neural SSL
comp and Mel Limiter already cover that ground. The gap is specifically the
**narrow, adaptive, relative-to-envelope** suppression that none of the current
stages do.

---

## 8. Sources

- Referenced codebase — Polarity-SC-Dark (re-skin of Robbert van der Helm's
  Spectral Compressor, nih-plug), DSP in `plugin/src/compressor_bank.rs`,
  `curve.rs`, `match_curve.rs`: <https://github.com/polarity/plugin.nih.polarity-sc-dark>
  and product page <https://polarity.productions/polarity-sc/>
- D. Giannoulis, M. Massberg, J. D. Reiss — *Digital Dynamic Range Compressor
  Design: A Tutorial and Analysis* (the soft-knee formula used per-bin).
- oeksound soothe2/soothe3 — dynamic resonance suppressor (mechanism):
  <https://oeksound.com/plugins/soothe2/>
- Baby Audio — *Resonance Suppressor vs. Multi-band Compressor vs. Dynamic EQ*:
  <https://babyaud.io/blog/resonance-suppresion-vs-multiband-compression> and
  *What is a Dynamic Resonance Suppressor*:
  <https://babyaud.io/blog/dynamic-resonance-suppressor>
- iZotope — *Resonant frequencies: what they are and how to fix them*:
  <https://www.izotope.com/en/learn/resonant-frequencies>
- KERN Audio — *Spectral compression explained*:
  <https://kernaudio.io/guides/compression/spectral-compression-explained>
- KVR — *How do "spectral" (FFT-based) dynamics processors work?*:
  <https://www.kvraudio.com/forum/viewtopic.php?t=531986>
- Sonarworks — *Pro Mastering: Dynamic EQ and Multiband Compression*:
  <https://www.sonarworks.com/blog/learn/pro-mastering-dynamic-eq-and-multiband-compression>
- Gearnews — *Resonance Suppression: Essential Techniques*:
  <https://www.gearnews.com/resonance-suppression-workshop-studio/>

*Axon cross-references:* [auto_eq_spectral_mask.md](auto_eq_spectral_mask.md)
(the STFT engine to reuse), [mel_limiter.md](mel_limiter.md),
[spectrum_analyzer.md](spectrum_analyzer.md),
[neural_inference_ort.md](neural_inference_ort.md).
