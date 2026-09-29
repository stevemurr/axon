# The Broad EQ voicing: a console EQ fitted from 8,300 measured transfer functions

The EQ stage has two voicings behind one set of knobs. **Classic** is the RBJ
cascade described in [`ssl-channel-eq.md`](ssl-channel-eq.md). **Broad**
(`SEQ_TYPE = 1`, "EQ Type" in the GUI) is a grey-box model of an **AMEK 9099**
channel EQ. Its shapes come from measured transfer functions: the same "use
the measured curves, fit a biquad model" route the Classic strip took, but
fitted end to end from an `eqds` dataset.

- DSP: `native/clap/src/amek_eq.hpp` (section designs) +
  `amek_eq_fit.hpp` (generated coefficient tables)
- Engine hook: `SslChannelEq::design_amek_` in `ssl_channel_eq.hpp`
  (`EqVoicing::Amek`)
- Fitter: `scripts/fit_amek_eq.py` (writes the tables + test golden vectors)
- Tests: `native/clap/tests/test_amek_eq.cpp`, plus the `SEQ_TYPE` checks in
  `test_ssl_integration.py` and `test_control_contract.cpp`

## 1. The data

`eqds` run `amek9099-v1-55884e47`
(`/Volumes/External/Data/eq-dataset/runs/…`, built from
`~/Code/github.com/stevemurr/eq-dataset`, profile `devices/amek9099`) rendered
the EQ and filters of *bx_console AMEK 9099* v1.3.1 at 48 kHz. Everything else
in that strip was off (dynamics, THD, noise, width). It contains 8,300
settings: 7,500 train, 500 eval (independent RNG, eval-only dry clips) and 300
diagnostic identity/replicate settings. Each setting carries the plugin's knob
read-back plus `qa.tf_mag_db`, a 256-point log-spaced magnitude response
(10 Hz–24 kHz) deconvolved from an exponential sweep. That column is exactly
what `eqds tf export` emits. The fitter reads `manifest.parquet` directly
because it needs the TFs joined to the knob read-back.

A flat −0.062 dB console level is removed from every curve. EQ-in with all
bands flat measures within 0.03 dB of flat, so Broad at 0 dB is modelled as
exact identity.

## 2. What the measurements say, section by section

Per-example free fits of candidate section families over 20 Hz–20 kHz
(train split, median RMS):

| Section | Best family | Median RMS | Character found |
|---|---|---|---|
| LMF / HMF bell | RBJ peaking, **bilinear** | 0.055 / 0.032 dB | Constant-Q, and broad: effective RBJ Q ≈ 0.31 × panel Q (panel 0.65–2 → Q 0.2–0.65). The HMF cramps to exactly 0 dB at Nyquist, like any bilinear bell |
| LF / HF bell | analog peak, **decramped** | 0.068 / 0.040 dB | **Proportional Q**: Q rises from ≈0.36 at low gain to ≈0.6 at ±18 dB. The HF bell keeps its gain up to Nyquist (a bilinear bell would force 0 dB there, which costs up to 3.5 dB) |
| LF / HF shelf | general 2nd-order (separate zero/pole Q), decramped | 0.033 / 0.031 dB | A resonant knee: the LF shelf overshoots its plateau by ~1.5–2 dB just inside the corner at deep cuts. An RBJ shelf scored 0.10–0.24 dB |
| HPF | 2nd-order HP + 1st-order HP | 0.135 dB | 18 dB/oct, near-Butterworth |
| LPF | two 2nd-order LP sections, decramped | 0.043 dB | ~18–19 dB/oct asymptote with a small pre-corner bump. A four-pole fit wins only because of the bump; one pair collapses to an effective first-order pole |

The panel's frequency legends are **not** a smooth function of the pot. The
×2/÷2/×3/÷3 range switches collapse onto one smooth curve of true frequency
versus pot position. The labels wiggle ±15–30 % around it, and the HF shelf's
label sits 2–5× above its actual midpoint. The panel Q legend follows an
S-shaped taper.

## 3. Knob semantics (why the A/B toggle keeps bands in place)

Axon has no pot, so the panel quirks are not shipped. In both voicings:

- **freq** = where the section physically acts: bell centre, shelf midpoint
  (geometric centre of the pole/zero pairs; the RBJ shelf's f0 is the same
  thing), or the filter's −3 dB point.
- **gain** = peak gain (bells) or asymptotic plateau gain (shelves).
- **LMF/HMF Q** stays on the console's own broad scale (Q_rbj ≈ κ·Q_knob,
  κ = 0.318 LMF / 0.308 HMF, with a small fitted gain term). At default Q 1.0
  a Broad bell is about three times wider than a Classic one. That is the
  character.

Flipping Classic ↔ Broad therefore changes shapes, not placement. The plugin
integration test checks this: an LMF +9 dB at 1 kHz reads +8.99 dB in both
voicings, while 100 Hz reads +0.11 dB (Classic) vs +0.88 dB (Broad).

Knob values outside what the console can reach extrapolate. Frequencies scale
exactly, and the shape laws clamp at the measured physical range: LF shelf
98–444 Hz, HF shelf 0.73–4.6 kHz, LF bell 33–294 Hz, HF bell 2.2–29 kHz,
LMF 48–948 Hz, HMF 0.49–10 kHz, HPF 18 Hz–1 kHz, LPF 1.5–19 kHz.

## 4. Decramping: matched biquads that survive cuts and Nyquist

The decramped sections (`amek::matched`) place poles by impulse invariance and
match the numerator's squared magnitude to the analog prototype. With
φ1 = sin²(ω/2), φ0 = 1 − φ1, φ2 = 4φ0φ1:

    |B(e^jω)|² = B0·φ0 + B1·φ1 + B2·φ2

B0 and B1 are pinned to the analog DC and Nyquist gains. B2 is a closed-form,
relative-error least-squares fit over 16 log-spaced points, which is better
than the textbook single third point. `b0..b2` are then recovered in
minimum-phase form.

The naive version (third-point match, poles taken from H as given) was up to
57 dB wrong. Cut bells and falling shelves have overdamped or high poles that
impulse invariance places badly. The fix is to always discretize the lower /
sharper of H's two pole pairs. When H's zeros are that pair, design 1/H and
invert the result, which is exact because every section is minimum phase.
Worst-case deviation from analog then drops to 1.3 dB (broad bells right at
44.1 kHz Nyquist, the limit for any 2nd-order section), 0.15 dB median, and
0.3 dB at 96 kHz. Real poles are computed cancellation-free, and poles are
clamped to 0.45·fs.

## 5. The fit

`scripts/fit_amek_eq.py` runs in three stages:

1. **Per-example fits** of each family, using the exact digital designs the
   C++ runs, on single-section train examples.
2. **Shape laws**: small polynomials in u = log2(f/f_ref) (clamped) and
   v = g/18. These give the mid-bell Q gain term and κ (via a piecewise-linear
   map of the panel's Q taper), the proportional-Q bell laws, shelf zero-Q and
   pole-Q, and the filter section spacing and Qs. Shelf pole-Q is written as
   zero-Q·exp(v·…), so a 0 dB shelf is exactly flat (an earlier
   parameterisation left a 0.35 dB bump).
3. **Joint refinement** of each law directly against the measured curves,
   with each example's knob values (freq, gain) as nuisance parameters. Filter
   knobs are pinned to the measured −3 dB point, because a free cutoff trades
   off against the law's own frequency offset. Before that was fixed, the
   drift pushed the held-out p90 from 0.6 dB to 4.9 dB.

The panel-label → knob maps (the frequency-legend wiggle, the Q taper, the
gain law) are fitted on train only. They are used solely to score the model on
eval settings and never ship.

## 6. Results (held-out eval split, 500 settings, all band/filter combinations)

Per-example RMS error over 20 Hz–20 kHz, ignoring stop-band below −40 dB:

| | median | p90 | max |
|---|---|---|---|
| **Broad model** | **0.25 dB** | **0.59 dB** | 2.1 dB |
| Broad with panel labels used as knobs | 1.63 dB | 5.0 dB | 9.5 dB |
| Classic voicing at the same knobs | 2.81 dB | 4.8 dB | 9.3 dB |

Single-section medians: HMF 0.06, LF bell 0.07, LMF 0.08, HF bell 0.08,
LF shelf 0.11, HF shelf 0.16 dB. Error grows gently with band count (median
0.12 dB for filters alone up to 0.32 dB with all four bands), consistent with
a plain series cascade. The residual tail is mostly the eval-only label maps:
for the worst LF-shelf cases, re-fitting just the two knob values brings the
shipped model to 0.05–0.19 dB.

## 7. Tests

`test_amek_eq.cpp` checks:

- The C++ reproduces the fitter's golden vectors to < 1e-4 dB (measured
  6e-7) at 44.1/48/96 kHz.
- It matches 16 held-out *measured* console curves: median RMS < 0.4 dB
  (0.26), max < 2.5 (2.1), with Classic ≥ 3× worse.
- Flat Broad is bit-transparent, and a 0 dB shelf designs flat (6e-10 dB).
- 6,630 section designs are strictly stable from 22.05 to 192 kHz over every
  knob range.
- `process()` matches `magnitude_db()` within 0.013 dB (Goertzel).
- Decramped sections agree across 44.1 k and 96 k (≤0.66 dB above −20 dB). The
  HMF is intentionally rate-dependent near Nyquist.
- Both filters measure ~18 dB/oct.
- The duplicated RBJ helpers stay bit-identical to `ssl_design`.
- Switching voicing clears filter state.
- The calibration solver recovers Broad-shaped gains within 0.14 dB.

## 8. Plumbing

- `SEQ_TYPE` is a stepped `enum` (`classic`/`broad`, default Classic, so
  existing sessions keep their sound). It is read in `resolve_amount_` and
  applied to all three banks. It is declared in `composite.py`, the shipped
  meta and the load-time `inject()`, and gets host value↔text names like
  `SEQ_MODE`.
- `SslEqParamsRT::voicing` joins the change guard. A voicing change clears
  filter state, like a mode change, because the sections change meaning.
- **Recalibrate** fits the active voicing's band shapes (`SslSolverBand` gains
  `voicing`/`slot`).
- The 6 assist bells and the Colour stage are voicing-independent.
- GUI: a two-button stack selector ("EQ TYPE") in the HPF column's free slot.
  A tenth column would not fit the fixed-width surface.
- Cost: Broad runs 8 core sections, Classic still runs 7 (the 8th slot is
  skipped). Designs are recomputed only on knob changes: each matched
  section costs ~20 transcendental calls (its 16-point grid).

## 9. Limits and honest loose ends

- The reference is *bx_console's emulation* of the console, not hardware, and
  its THD/noise were off. Broad is a linear voicing only; the Colour knob is
  still Classic's (inert) waveshaper.
- Outside the measured ranges (§3) the shapes are extrapolations by clamping.
  In particular, an HF shelf midpoint above ~4.6 kHz uses the 4.6 kHz shape.
- At ±18 dB near the top of its range the LF shelf's knee overshoots the
  plateau by 2–3.7 dB. That is the console's character, not an artifact.
- Mid bells are bilinear at the host rate, as the console is at 48 kHz, so
  their top octave cramps more at 44.1 k than at 96 k.
- Knob Q below the panel's 0.65 extrapolates to very broad bells
  (Q_rbj ≈ 0.03 at knob 0.1).

## Refitting

    uv run scripts/fit_amek_eq.py --run /Volumes/External/Data/eq-dataset/runs/amek9099-v1-55884e47

This takes about 30 s. It prints the validation report and regenerates
`native/clap/src/amek_eq_fit.hpp` (the report is embedded in its header
comment) and `native/clap/tests/amek_eq_golden.hpp`. Use `--no-write` to score
only, and `--dump out.json` for the tables and eval metrics.
