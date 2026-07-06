# The Channel EQ: an SSL 9000 J strip in 13 biquads, with a seqlock-coupled auto-calibration that turns the knobs for you

Axon's chain is mostly learned: an LSTM drives the Auto-EQ, a TCN emulates the
bus compressor, the limiter solves per-band gains from a Mel spectrum. Stage 2
— the GUI just calls it **"EQ"** — is the one classic-console block in the
lineup: an SSL 9000 J channel-strip EQ built from nothing but RBJ-cookbook
biquads (`native/clap/src/ssl_channel_eq.hpp`). It exists for two reasons that
pull in opposite directions:

1. **It must be boring.** The cascade mirrors the training-side
   `nablafx.processors.SSLConsoleEQ` (and its `dsp.biquad` designer)
   *exactly*, coefficient for coefficient, so that parameters learned by the
   grey-box console model transfer to the plugin bit-for-bit
   (`ssl_channel_eq.hpp:1-26`). No clever filter topology, no decramped
   high-end, no oversampling — the RBJ cookbook, a0-normalized, because that
   is literally what the Python trainer computes.
2. **It must not be boring to use.** On top of the manual strip sits a
   linear-algebra party trick: press **Recalibrate** and a closed-form
   least-squares solver fits the four band gains to the Auto-EQ's current
   correction curve and *writes the result onto the visible knobs* — you watch
   the EQ voice itself. The main-thread solver hands data to the audio thread
   through a hand-rolled seqlock whose memory-ordering was formally hardened
   in commit `158b6c8`.

This document covers the cascade math, the shelf↔bell morph, the solver, the
seqlock, the two generations of the calibration feature, the measured cost —
and one honest loose end: the Colour knob currently does nothing, and we can
prove it.

```
             ┌───────────────────────── "SSL character core" (7 biquads) ─────────────────────────┐
 in ──► HPF ──► HPF ──► LF (shelf⇄bell) ──► LMF (bell) ──► HMF (bell) ──► HF (shelf⇄bell) ──► LPF ──►
       (×2 identical 2nd-order sections)                                                           │
                                                                                                    ▼
                                                                      Colour: (1−mix)·x + mix·P(x)/Q(x)
                                                                      (RationalA — currently inert, §5)
                                                                                                    │
                                                                                                    ▼
             ┌──────────────── 6 "assist" bells: 60 / 175 / 500 / 1400 / 4000 / 11000 Hz, Q 1.4 ───┘
 out ◄───────┴─ (solver-driven, currently dormant — §7)
```

7 SSL sections + 6 assist bells = `kNumBq = 13` biquads per channel
(`ssl_channel_eq.hpp:136-138`), all run every sample, identity-coefficient
sections included.

---

## 1. Where it sits, and why *before* the Auto-EQ

The stage is `StageID::SslEq = 3`, reusing the slot freed by the removed
OutputLeveler (`axon_plugin.cpp:106-117`), and the default order is
BassMono → **SslEq** → AutoEQ → Reverb → Widener → SslComp → MelLimiter
(`kDefaultStageOrder{6,3,1,8,9,4,5}`, `axon_plugin.cpp:125`).

Sitting *before* the Auto-EQ is load-bearing: whatever the channel EQ does is
seen by the adaptive Auto-EQ controller downstream, which then only solves the
*residual* (`axon_plugin.cpp:710-716`). This is what makes the calibration
loop converge without any explicit feedback bookkeeping — the "plant" (the
adaptive EQ) contracts the error for free (§7).

Two properties every other stage envies:

- **Zero latency.** Minimum-phase IIR; `latency_samples()` returns 0
  (`ssl_channel_eq.hpp:201`). Nothing to align in the bypass FIFO.
- **Sample-rate correct.** Coefficients are recomputed at the *host* rate
  from physical (freq, gain, Q) inside `set_params`. This is the one EQ-ish
  stage in Axon that is **not** silently locked to 44.1 kHz — the FFT/ONNX
  stages run fixed sample counts trained at 44.1 k and drift at other rates
  (issue #11); this stage's response is provably rate-invariant, and
  `test_sample_rate_correct` asserts the magnitude at 1 kHz matches across
  44.1/48/96 k within 0.05 dB (`tests/test_ssl_channel_eq.cpp:89`).

Defaults make the stage a **bit-identical no-op**: `SEQ_ON` defaults off and
the flush case breaks before touching the buffer (`axon_plugin.cpp:1534`,
control injection `:2798-2823`). `test_ssl_integration.py` proves it on the
real built plugin: with `SEQ_ON=0`, cranking `SEQ_LF_G=12` changes *zero*
output bits (`tests/test_ssl_integration.py:98-101`).

---

## 2. The cascade: RBJ cookbook, a0-normalized, deliberately unoriginal

### 2.1 The designer

`ssl_design(type, gain_db, freq_hz, q, fs)` (`ssl_channel_eq.hpp:61-105`) is a
verbatim transcription of the RBJ Audio-EQ-Cookbook, with types 0 peaking,
1 low-shelf, 2 high-shelf, 3 high-pass, 4 low-pass. The shared intermediates:

```
A     = 10^(gain_db/40)          (amplitude, √ of the linear gain)
w0    = 2π·f/fs
alpha = sin(w0) / (2Q)
```

e.g. peaking:

```
b0 = 1 + alpha·A     b1 = −2·cos(w0)     b2 = 1 − alpha·A
a0 = 1 + alpha/A     a1 = −2·cos(w0)     a2 = 1 − alpha/A
```

then **everything is divided by a0 before storage**
(`bq.set(b0/a0, …)`; `ssl_channel_eq.hpp:103`). Inputs are clamped, not
rejected: frequency to [1 Hz, 0.49·fs] and Q ≥ 1e-3 (`:63-66`), so a knob
mapped past Nyquist at a low sample rate degrades gracefully instead of
producing an unstable section.

Why the ceremony about a0-normalization? Because the *training* code
normalizes at the same point, and the grey-box console model learned its
(freq, gain, Q) parameters against that exact arithmetic. Any "equivalent"
reformulation — Horner tricks, direct-form-I, decramping — would make trained
parameters land on a slightly different response and quietly break the
transfer. The same discipline applies downstream: the filter state struct is
the shared `BiquadTDF2` (transposed direct form II, double precision,
`biquad.hpp:19-32`), extracted verbatim from the modules that used to
copy-paste it, with the explicit contract that `process()` bodies stay
token-identical so renders remain byte-identical (`biquad.hpp:1-11`).

`SslBiquad` adds one thing to `BiquadTDF2`: a complex-evaluation magnitude
probe `mag(w) = |b0 + b1·z⁻¹ + b2·z⁻²| / |1 + a1·z⁻¹ + a2·z⁻²|` at
`z⁻¹ = e^(−jw)` (`ssl_channel_eq.hpp:46-56`). The header warns it must not be
merged with `IirFilterbankEq`'s trig-based `mag_db()` — they are different
evaluations with different rounding, each matching its own consumer
(`biquad.hpp:9-11`). `magnitude_db(hz)` multiplies all 13 section magnitudes
and takes 20·log10, flooring at 1e-12 (`ssl_channel_eq.hpp:194-199`); it is
both the UI curve source and the solver's ground truth, and
`test_process_matches_magnitude` pins it to the actual audio path within
0.15 dB via a Goertzel probe (`tests/test_ssl_channel_eq.cpp:75`).

### 2.2 The HPF: the comment says 18 dB/oct, the code says 24

`design_()` builds the high-pass as **two identical RBJ 2nd-order sections**:

```cpp
coeff_[0] = p.hpf_on ? ssl_design(3, 0, p.hpf_hz, p.hpf_q, sr_) : identity_();
coeff_[1] = coeff_[0];   // second HPF section (18 dB/oct total)
```
(`ssl_channel_eq.hpp:220-221`)

That comment is wrong, and it's worth being precise because the number gets
repeated. Each section is a 2nd-order high-pass at Q = 1/√2 (the Q is pinned
to 0.70710678 in `resolve_amount_`, `axon_plugin.cpp:1477`, regardless of any
future knob). Two cascaded Butterworth sections are a textbook 4th-order
Linkwitz–Riley: numerically evaluating the shipped coefficients at
fc = 100 Hz gives −6.02 dB at the cutoff and a measured asymptotic slope of
**24.1 dB/oct** (−72.3 dB at 12.5 Hz → −96.3 dB at 6.25 Hz). The hardware
9000 J's high-pass is an 18 dB/oct (3rd-order) design — presumably what the
comment intended to invoke — but what ships, and crucially what the *training
code* computes, is 24 dB/oct LR4. Since fidelity-to-training beats
fidelity-to-hardware everywhere else in this stage, the code is right and the
comment should be fixed, not the filter.

### 2.3 The shelf↔bell morph: linear in coefficient space

The 9000 J's LF and HF bands have a BELL switch. Axon generalizes it to a
continuous `bellmix ∈ [0,1]` and morphs by **linearly blending the two
a0-normalized coefficient sets**:

```cpp
o.set((1−mix)·shelf.b0 + mix·bell.b0,  …,  (1−mix)·shelf.a2 + mix·bell.a2);
```
(`blend_`, `ssl_channel_eq.hpp:209-217`)

At mix 0 and 1 this is exactly the RBJ shelf / peaking filter. In between it
is *not* any cookbook prototype — it's a smooth, differentiable path between
the two, and that differentiability is the point: it matches the training-side
`SSLConsoleEQ`, where the blend lets gradients flow through the shelf/bell
choice during grey-box optimization. (A caveat for filter theorists: a convex
combination of two stable denominators is not stable *in general*, but for
same-frequency, same-gain RBJ shelf/bell pairs the blended poles stay well
inside the unit circle; and the plugin currently only ever feeds 0 or 1 —
`resolve_amount_` binarizes the switch at 0.5, `axon_plugin.cpp:1479,1482`.)
`test_shelf_vs_bell` pins the audible contract: an LF shelf at +10 dB holds
+6 dB at 20 Hz while the bell has fallen back under +2 dB
(`tests/test_ssl_channel_eq.cpp:104`).

### 2.4 Redesign discipline

`set_params` is **change-guarded**: `SslEqParamsRT` carries a field-by-field
`operator==` (`ssl_channel_eq.hpp:119-128`) and the engine early-returns if
the resolved params are identical to the last design (`:168-172`). Exact
float equality is correct here — it's a cache key, not a tolerance. When a
redesign does happen, the new coefficients are copied into both channels'
sections **preserving each channel's z-state** (`:235-238`), so twisting a
knob mid-note doesn't click from a state reset. `reset()` clears state and
invalidates the guard so the next `set_params` always redesigns (`:155-160`),
verified by `test_reset_clears_state` (`tests/test_ssl_channel_eq.cpp:150`).

The per-sample hot loop is dead simple (`process_ch_`,
`ssl_channel_eq.hpp:254-263`): promote to double, run sections 0-6 (the SSL
core), optionally crossfade the Colour waveshaper, run sections 7-12 (the
assist bells), narrow to float. Stereo is two independent mono passes over
per-channel state (`:186-190`).

---

## 3. The control surface: 22 `SEQ_*` ids under contract

The stage exposes 22 controls, injected with safe defaults at load so old
bundles acquire them automatically (`axon_plugin.cpp:2798-2823`): `SEQ_ON`
(default **off** — the bit-identical bypass), per-band gain/freq (+Q for
LMF/HMF, +BELL switch for LF/HF), HPF/LPF on/off + cutoff, `SEQ_DRIVE`
("Colour"), and the calibration cluster `SEQ_AUTO` ("Auto Assist"),
`SEQ_SPLIT` (default 0.6), `SEQ_CAL` ("Recalibrate", momentary), `SEQ_RESET`
(momentary). Gains span ±18 dB; LMF/HMF Q spans 0.1-4.

`resolve_amount_` maps them into the engine's `SslEqParamsRT`
(`axon_plugin.cpp:1426-1436`, `:1472-1488`), with LF/HF Q pinned at 0.707 and
`eq_on` always true when the stage runs (the master gate lives at the flush
case instead).

Because `export/composite.py` (which generates the shipped
`axon_meta.json`) has drifted from the C++ read-set more than once,
`tests/test_control_contract.cpp` regex-extracts every literal
`c.id == "..."` compare from `axon_plugin.cpp` and asserts the meta's control
set equals it exactly — no missing knobs (stage stalls at default), no dead
knobs — plus an explicit spot-check that all 22 `SEQ_*` ids exist, `SEQ_ON`
defaults off, and `SEQ_SPLIT` defaults 0.6
(`tests/test_control_contract.cpp:39-44,82-101`).

One rename to be aware of when reading history: commit `de69dab` de-branded
the GUI stage from "SSL EQ" to "EQ" (tab, stage names, toggle label). The
control ids stayed `SEQ_*`, so automation, saved sessions, and the meta↔C++
contract were untouched.

---

## 4. The solver: fitting band gains to a curve in closed form

`SslEqSolver` (`ssl_channel_eq.hpp:289-341`) answers: *given fixed band
shapes (type, centre, Q), what gains best reproduce a target dB curve?*

The trick that makes it closed-form is treating each band's dB response as
**linear in its gain knob**. Build the basis by designing each band once at a
reference gain (6 dB) and normalizing:

```
B[k][b] = dB_response( band b @ +6 dB, freq k ) / 6
```

For an RBJ peaking filter this linearization is exact at the band centre (the
centre response *is* `gain_db`) and very good off-centre at moderate gains —
the filter's bandwidth changes slightly with gain, which is where the
residual error lives. Then the least-squares fit of gains `g` to target `t`
is the normal-equations solve:

```
(BᵀB + 1e-6·I) g = Bᵀ t
```

with a tiny ridge for conditioning (`:334`) — the assist bells overlap, and
shelf pairs can trade broad tilt against each other, so BᵀB is
near-singular in the flat directions; 1e-6 is enough to keep the solution
finite without biasing audible gains. The B×B system (B = 4 or 6) is solved
by Gauss-Jordan elimination with partial pivoting and a 1e-18 pivot floor
(`solve_dense_`, `:343-365`), and the gains are clamped to ±`max_gain_db`
(`:337-338`).

`test_solver_recovers_gains` synthesizes a target from known gains
(3, −4, 2.5, −1.5 dB) and recovers them within 0.5 dB — the linearized basis
is near-exact at these magnitudes (`tests/test_ssl_channel_eq.cpp:166`).
`test_solver_clamps` feeds an absurd +40 dB target and checks the ±12 clamp
(`:223`).

Two prebuilt voicings exist:

- `SslEqSolver()` — 4 broad bands: 90 Hz low-shelf (Q 0.7), 500 Hz bell
  (Q 0.8), 3 kHz bell (Q 0.9), 10 kHz high-shelf (Q 0.7) (`:293-294`). Broad
  on purpose: it *can only* take coarse tilt/shelf moves, so it cannot fight
  the Auto-EQ's fine detail.
- `SslEqSolver::assist()` — bands = the engine's six assist bells, so the
  solve output maps 1:1 onto `set_assist_gains` (`:300-305`).

In the shipped plugin, neither is used verbatim for calibration: the solve
constructs its bands from the **user's current knob settings** (§7).

---

## 5. The Colour knob: real math, missing coefficients

`SEQ_DRIVE` crossfades a `RationalA` waveshaper into the cascade between the
SSL core and the assist bells:

```cpp
if (harm) x = (1.0 − hmix)·x + hmix·rational_.eval(x);
```
(`ssl_channel_eq.hpp:260`)

### 5.1 The waveshaper (the math worth keeping)

`RationalA` (`rational_a.hpp`) mirrors the PyTorch
`rational.torch.Rational_PYTORCH_A_F` — a *learnable rational function*, i.e.
a ratio of polynomials whose coefficients are fitted rather than hand-tuned:

```
P(x) = a₀ + a₁x + a₂x² + … + aₙxⁿ            (numerator: n+1 coeffs)
Q(x) = 1 + |b₁x| + |b₂x²| + … + |bₘxᵐ|       (denominator: m coeffs, b₀ pinned to 1)
y    = P(x) / Q(x)
```

What makes it version "A" is the denominator: the constant term is pinned to
exactly 1 (removing a redundant global-scale degree of freedom, and why the
stored denominator starts at b₁), and every term is wrapped in absolute
value, so Q(x) ≥ 1 for all real x — **poles on the real axis are
algebraically impossible**. A rational function saturates naturally (the
denominator catches up with the numerator at large |x|), which is why a
handful of coefficients can imitate tanh-like soft clipping or asymmetric
tube curves; the |·| guarantee is what makes that safe against arbitrary
loud input on an audio thread.

Evaluation (`rational_a.hpp:38-55`) is all in double: Horner's method for the
numerator (n multiply-adds, no `pow`), but the denominator builds powers
term-by-term because each term needs its own `fabs` *before* summing — a
Horner refactor would change the math and diverge from the PyTorch
reference. No allocation, no branches beyond the loop, no transcendentals:
a couple dozen FLOPs plus one divide per sample. Gotchas carried over from
the original write-up, still true: coefficient lengths are never validated
(`reset()` copies whatever it's given); prepending a b₀ to the denominator
array silently shifts every power; an empty numerator yields constant 0, not
passthrough.

The same class is the engine of the **dormant Saturator** stage
(StageID 2, kept in code but not in `processor_order`;
`axon_plugin.cpp:94-100`), where trained coefficients ship in the
`saturator` sub-bundle and the stage wraps the curve with drive / makeup /
wet-mix / band-split / threshold / bias controls (`SDR/SVO/SMX/SHF/SLF/STH/SBS`,
resolve at `:1401`, process case at `:1747-1824`). Whether that stage
returns or is deleted is an open decision
(`docs/future/not-started/saturator-removal-decision.md`), which notes
`rational_a.hpp` must stay either way — because of this Colour stage.

### 5.2 The honest finding: it's a no-op on `origin/main`

The gate in the hot loop is:

```cpp
const bool harm = hmix_ > 0.0 && !rational_.empty();
```
(`ssl_channel_eq.hpp:256`)

and coefficients only arrive via `set_harmonic(num, den)`
(`:164-166`). Grep the tree: **`set_harmonic` is called exactly twice, both
in the unit test** (`tests/test_ssl_channel_eq.cpp:129,217`). The plugin
never loads coefficients into `ssl_eq` — not at activate, not from any
bundle. So `rational_.empty()` is always true in the shipped plugin, `harm`
is always false, and the Colour knob resolves into `harmonic_mix`
(`axon_plugin.cpp:1483`) that changes nothing. The README's "harmonic Colour
control" is, today, a knob wired to an empty vector.

This is a *latent* feature, not a bug in the DSP: `test_harmonic_stage`
proves the machinery works when fed (mix=0 is bypass-identical; a loaded
cubic-ish curve produces a 3rd harmonic >50× the input's;
`tests/test_ssl_channel_eq.cpp:126`). A handoff doc shipped with the
calibration commit (`native/clap/docs/ssl_harmonic_training_handoff.md`,
added in `6265fc3`) spelled out the completion plan — its key insight: for a
*memoryless* shaper the transfer function **is** the model, so no ML training
is needed; fit `num`/`den` by least squares to a designed target curve like
`f(x) = tanh(k·(x + β·x²))/tanh(k)` (small k for gentle drive, small β for
even-harmonic asymmetry), enforce f(0)=0 and unit slope at 0, and install via
`set_harmonic` at activate (embedded constants or a tiny `ssl_harmonic`
sub-bundle mirroring the saturator's). ML would only buy memory /
frequency-dependence, which `RationalA` cannot represent anyway. That doc was
then deleted in the `7d04964` docs cleanup ("task-based docs die once
consumed") — but it was consumed *without being implemented*, and no
`docs/future/` idea doc replaced it. The plan now lives only in git history
and in this paragraph.

Deliberate placement detail: the assist bells run **after** the Colour stage
(`ssl_channel_eq.hpp:259-261`), so if/when Colour goes live, the
solver-driven linear correction applies to the *coloured* signal and the
fit stays honest — the character core plus its nonlinearity is the thing
being corrected, not the other way around.

---

## 6. The seqlock: main thread solves, audio thread ramps

The calibration solve runs on the **main thread** (it does heap-allocating
linear algebra — vectors of vectors, Gaussian elimination — that has no
business in a CLAP audio callback). Its results must reach the audio thread
without locks. The mechanism is a classic seqlock
(`axon_plugin.cpp:775-786`):

```cpp
std::atomic<uint64_t> ssl_asg_gen{0};                    // even = stable, odd = writer mid-update
std::array<float, kNumAssist> ssl_asg_published{};      // the payload (not atomic)
```

**Writer** (main thread, `axon_plugin.cpp:2217-2221`):

```
gen.store(gen+1, release)            // odd: "under construction"
atomic_thread_fence(release)         // order the data stores AFTER the odd marker
write ssl_asg_published[…]
gen.store(gen+2, release)            // even: data ordered before by the release store
```

**Reader** (audio thread, `axon_plugin.cpp:1540-1546`; identical copy in the
main-thread viz path `:2608-2614`):

```
for tries in 0..3:
    g0 = gen.load(acquire)
    if g0 is odd: retry               // writer mid-update
    copy ssl_asg_published            // plain loads
    atomic_thread_fence(acquire)      // gains read BEFORE the re-check
    if gen.load(acquire) == g0: done  // unchanged generation ⇒ copy is coherent
```

The two `atomic_thread_fence` calls are the fix from commit `158b6c8`, and
the bug they close is a nice weak-memory-model case study: the original
writer published the odd counter with a plain `release` store — but a release
store only orders operations *before* it. The subsequent non-atomic gain
writes were free to become visible **ahead of** the odd marker on a
weakly-ordered CPU (arm64, i.e. every Apple Silicon machine this runs on), so
a reader could observe half-written gains behind a still-even generation and
accept them. Mirror-image on the read side: the payload loads needed an
acquire fence before the generation re-check, or the re-check could complete
using stale-read reasoning. The commit message is candid that the practical
impact was almost nil — the payload feeds a 0.001-per-block one-pole, writers
fire at most ~1/s, and the retry loop usually catches it — "but now correct."
Textbook seqlock fencing, four lines, and the kind of thing you fix while
it's cheap.

Failure mode honesty: after 4 failed tries the reader proceeds with whatever
it last copied (possibly zeros). Given microsecond writes and ~1 Hz writers
this is essentially unreachable, and the downstream smoothing makes even a
torn read inaudible by construction.

**The ramp.** The audio thread never applies published gains directly. Each
channel keeps `ssl_asg_smooth`, advanced once per 128-sample flush:

```cpp
sm[b] = 0.999f·sm[b] + 0.001f·(amt.ssl_auto · asg[b]);
e.set_assist_gains(sm.data(), kNA);
```
(`axon_plugin.cpp:1552-1557`)

A one-pole with τ = 1000 blocks ≈ **2.7 s at 48 kHz** (2.9 s at 44.1 k): a
new solve *glides* in, and `set_assist_gains` change-guards per band and
redesigns only the six assist biquads when something actually moved
(`ssl_channel_eq.hpp:177-184`, `design_assist_` `:245-252`) — cheap, and
z-state is preserved so the glide doesn't zipper.

---

## 7. Calibration, two generations: from hidden contraction to visible knobs

### 7.1 v1 (port, commit `0d0d36b`): the invisible assist layer

The original coupling fit the **six hidden assist bells** to the Auto-EQ
curve every ~1 s (a 21-tick timer at the ~21 fps UI cadence) or on a
`SEQ_CAL` edge, accumulated `SEQ_SPLIT` of the *current residual* into the
published gains each solve, and let the geometry do the rest: SSL absorbs
α of the curve → the Auto-EQ (downstream, adaptive) re-solves a smaller
residual → next timer tick absorbs α of *that* — geometric contraction
toward the SSL owning the whole correction. The design note in the code
called it "static solve … fixed beats dynamic" (`axon_plugin.cpp:775-778`):
fixed band centres/Qs re-solved slowly, rather than filters that chase the
curve block-by-block, because slowly-ramped fixed filters can't warble,
zipper, or ring along with FFT jitter.

That comment still cites `docs/ssl_eq_coupling.md` for the rationale — a doc
that **never existed in this repository's history** (dangling since the port
commit; presumably it lived in the branch/worktree the port was taken from).
The paragraph above is the reconstruction; the citation should be fixed or
dropped.

v1's problem was product, not math, and the calibration commit `6265fc3` is
blunt about it: six narrow (Q 1.4) bells fit to an FFT-derived curve are
**jagged and ringy**, the layer is **invisible** (nothing on the UI moves),
and the auto-contraction marches toward *full cancellation* of a stage whose
whole identity is transparency.

### 7.2 v2 (shipped, `6265fc3`): "musical calibration" — the knobs are the integrator

`solve_ssl_coupling_` (`axon_plugin.cpp:2199-2256`) now does, on a `SEQ_CAL`
rising edge only (`:2226-2228`; gated on `SEQ_AUTO > 0` and the stage being
on, `:2225`):

1. **Build the target**: 50 log-spaced points, 20 Hz-20 kHz,
   `tgt[k] = SEQ_SPLIT · mt_eq_bins[k]` (`:2230-2235`). `mt_eq_bins` is the
   Auto-EQ's *actual rendered curve* in dB — latched on the audio thread from
   the active renderer (IIR bank `magnitude_db` or STFT mask
   `sample_gains_db`, `axon_plugin.cpp:1717-1720,1736-1739`) and shipped
   through the spectrum-analyzer transfer every 2048-sample window.
2. **Build the solver bands from the four real, visible bands at their
   current settings** — LF/HF follow the shelf↔bell switch, LMF/HMF carry
   their live freq/Q (`:2237-2246`). The calibration respects however you've
   voiced the strip.
3. **Solve** with a per-press cap of ±9 dB (`:2247`).
4. **Add the solved deltas onto the visible gain knobs**, clamped to the
   ±18 dB control range, via `set_visible_param_` (`:2250-2255`) — which
   pushes through the same GUI→audio `param_queue` a human knob twist uses
   *and* evals `axonSetParam(...)` into the WebView so the knob graphic moves
   (`:2188-2197`).

The state of the calibration *is the knob positions*. One press absorbs one
partial step (`SEQ_SPLIT` of the current curve, default 0.6); it deliberately
does **not** auto-contract on a timer toward full cancellation — the Auto-EQ
is transparent and is supposed to keep doing the residual. Press again to
absorb more; because the stage sits before the Auto-EQ, each press sees an
already-shrunk residual, so repeated presses converge geometrically anyway —
but under the user's finger, not a timer's. `SEQ_RESET` flattens the four
gain knobs and clears the dormant assist layer (through the seqlock writer),
running regardless of the `SEQ_AUTO` gate (`:2210-2223`).

Why four broad bands beat six narrow bells is *quantified*:
`test_musical_vs_assist_fit` fits both voicings to a deliberately wiggly
multi-scale target and computes the total variation of the fitted magnitude
over 400 log points — the 4-band musical fit must come in under 0.7× the
6-bell fit's TV (measured at the time of the commit: **3.3 → 1.7 dB**)
(`tests/test_ssl_channel_eq.cpp:236-269`).

The solve runs **headless**: `on_main_thread` pumps
`spectrum.process_if_ready()` and calls `solve_ssl_coupling_` *before* the
GUI-presence early-return (`axon_plugin.cpp:2586-2589`), so calibration works
with the editor closed (e.g. under `axon_bench` in the integration test).

### 7.3 What's left of the assist layer

On `origin/main` the six assist bells are **dormant by decision, live by
machinery**. The only seqlock writer left is the reset path (which publishes
zeros); the audio thread still does the seqlock read + ramp +
`set_assist_gains` every block whenever `SEQ_AUTO > 0` (`:1539-1557`), and
the change guard makes that free (zeros→zeros never redesigns). The commit
kept the layer explicitly to "re-enable later" — plausible future: knobs take
the musical move, assist bells take the fine residual on top.

---

## 8. The decomposition viz: Total = SSL + Auto EQ

Commit `bf95c53` made the calibration *visible*. `Plugin::ssl_viz_eq` is a
main-thread-only scratch `SslChannelEq` (`axon_plugin.cpp:783-786`) — never
touched by audio, so no locking questions. Each ~21 fps tick,
`on_main_thread` sets it to the current resolved params **plus** the
seqlock-read published assist gains scaled by `SEQ_AUTO`, then evaluates
`magnitude_db` at the same 50 log bins as the Auto-EQ curve and evals an
`axonSslCurve({on, bins})` payload into the WebView
(`axon_plugin.cpp:2597-2631`). The UI layers three curves on the spectrum:
the SSL contribution (dashed blue), the Auto-EQ's own curve, and the
prominent TOTAL = SSL + Auto EQ (`ui/index.html:1527-1580`) — so a
Recalibrate press shows the correction *migrating* from the Auto-EQ trace
into the EQ trace while the total holds still. Including the assist gains in
the viz path is why even the dormant layer would be visible the day it wakes
up.

---

## 9. Measured cost: 13 double biquads are nearly free

From the instrumented perf ranking (2026-07-05, buf = 128, all-stages
scenario; `native/clap/docs/perf_stage_ranking.md`):

| Metric | SslEq |
|---|---|
| share of process() @128 | **0.589 %** (0.582 % @512 — buffer-size invariant) |
| mean per block | **2.65 µs** |
| p95 / max | 4.02 µs / 79.88 µs |
| bypassed (`SEQ_ON=0`) | ~0.01 µs |

For scale: the two ONNX stages are 58.5 % (bus comp) and 33.5 % (Auto-EQ) of
the process budget. 2.65 µs buys 13 double-precision TDF2 biquads × 2
channels × 128 samples (≈ 3,300 biquad evaluations) plus the per-block
change-guard checks — roughly 0.8 ns per biquad-sample. That's the practical
argument for never optimizing this stage: it runs identity sections for
disabled bands rather than branching, redesigns at most 13 tiny filters on a
knob move, and still doesn't register. It also runs zero allocations, zero
locks, and zero syscalls on the audio thread (the solver, which allocates, is
main-thread-only by construction — §6).

---

## 10. Limitations, trade-offs, loose ends

- **The Colour knob is inert** (§5.2). `SEQ_DRIVE` is exposed, documented in
  the README, resolved, compared in the change-guard — and gated off by
  `rational_.empty()` because nothing ever calls `set_harmonic` outside the
  unit test. Either ship fitted coefficients (the deleted handoff's Path A is
  a ~30-line NumPy `lstsq` away) or pull the knob; a knob that does nothing
  is the worst of both. There is currently no `docs/future/` idea doc
  tracking this — the plan exists only in git history (`6265fc3`, deleted in
  `7d04964`).
- **Two stale comments in shipped code.** `ssl_channel_eq.hpp:221` claims the
  double HPF is "18 dB/oct total" — it is 24 dB/oct LR4, measured (§2.2). And
  `axon_plugin.cpp:778` cites `docs/ssl_eq_coupling.md`, which has never
  existed in this repo (§7.1).
- **Dormant machinery on the hot path.** The seqlock read + 6-band ramp +
  `set_assist_gains` run every block while `SEQ_AUTO > 0`, even though the
  published gains are pinned to zero on `origin/main`. Harmless (change
  guards eat it) but worth remembering when the assist layer is revived or
  deleted.
- **Calibration accuracy is bounded by its inputs.** The target is the
  Auto-EQ's rendered curve as of the last spectrum transfer (2048-sample
  window, latched every 8th flush) — press CAL during a transient program
  change and you calibrate to a snapshot. The solver's linear-in-gain basis
  is exact at band centres and drifts at large gains (the ±9 dB per-press
  clamp keeps it in the honest region); the fit is least-squares over a log
  grid, so it optimizes average dB error, not worst-case.
- **The morph mid-range is untrained territory.** `blend_` intermediate
  mixes (0 < mix < 1) are exact only at the endpoints; the plugin binarizes
  the switch, so the mid-range currently exists solely to mirror the
  differentiable training path. If a continuous shelf↔bell knob is ever
  exposed, its intermediate responses should be characterized first.
- **Deliberate non-features.** No oversampling and no decramped shelves —
  bit-compat with the trainer wins over analog-magnitude purism near Nyquist
  (the RBJ shelf/bell response cramps approaching fs/2, identically on both
  sides of the transfer, which is the point). Frequencies past 0.49·fs clamp
  rather than error. Per-press calibration will not converge to *zero*
  Auto-EQ residual by design — that's the transparency contract, not a bug.

## Superseded / related docs

- **Supersedes `docs/rational_a.md`** (2026-06-05): the Rational-A math,
  version-"A" denominator guarantees, Horner/`fabs` evaluation discipline,
  and coefficient-layout gotchas are carried into §5.1, reframed around the
  Colour stage (its live consumer-to-be). That doc's remaining unique content
  — the dormant Saturator stage's wrap-around controls and bundle loading —
  is summarized in §5.1 and otherwise lives with the saturator
  delete-vs-revive decision (`docs/future/not-started/saturator-removal-decision.md`).
- `README.md` "The chain" (stage 2 row) — user-level summary; unchanged and
  consistent with this dive except that "harmonic Colour" should be read
  through §5.2.
- `native/clap/docs/perf_stage_ranking.md` — source of the §9 numbers.
- Dangling: `docs/ssl_eq_coupling.md` (cited at `axon_plugin.cpp:778`, never
  existed) — §7.1 is its reconstruction.
