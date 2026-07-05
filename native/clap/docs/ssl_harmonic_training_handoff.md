# SSL harmonic "colour" — training/handoff for the SSL waveshaper

## Goal
Give the SSL 9000 J channel EQ an analog **colour** stage so it can add console
character (musical saturation) instead of only correcting. This is the "reverse
direction" the SSL-calibration work deferred: the auto-EQ keeps doing its transparent
correction, and the SSL contributes broad musical EQ **+ harmonic colour** voiced to
the material. Everything downstream is already wired — only the waveshaper coefficients
are missing.

## Where it plugs in (already built, currently inert)
- `native/clap/src/ssl_channel_eq.hpp` has a per-sample harmonic stage between the
  character core and the assist bands (`process_ch_`):
  `x = (1 - hmix)·x + hmix · rational_.eval(x)` — `hmix` is `SEQ_DRIVE` (the wet mix).
- The shaper is `nablafx::RationalA` (`src/rational_a.hpp`): a **memoryless rational
  function** `eval(x) = P(x) / Q(x)`, where `P(x)=a0+a1·x+…` and
  `Q(x)=1+Σ_j |b_j·x^j|` (the `|·|` keeps the denominator positive → BIBO-safe).
- Load coefficients via `SslChannelEq::set_harmonic(num, den)`; it's currently never
  called, so `rational_.empty()` is true and `SEQ_DRIVE` does nothing.
- The **saturator stage is precedent**: it's the *same* class (a nablafx
  "StaticRationalNonlinearity version A"), trained and shipped as
  `weights/axon_bundle/saturator/` — the model_id even reads `staticrationalnonlinearity`.

## Answer: "train the waveshaper, or learn the transfer function?"
For a **memoryless** shaper (which `RationalA` is), the transfer function *is* the
model — a static `input-sample → output-sample` curve. So:

- **You do NOT need ML training** to get a musical static colour. You can **fit the
  rational coefficients directly to a target transfer curve** (a plain curve-fit).
  "Train the waveshaper" and "learn the transfer function" are the same operation here.
- **Training on audio (ML) only buys you memory / frequency-dependence** — transformer
  hysteresis, level- or frequency-dependent harmonics. A static waveshaper *cannot
  represent* those no matter how it's fit, so ML is only worth it if you move to a
  model **with state** (Path C). For SSL "warmth," a static curve is the standard and
  is very likely sufficient.

## Paths (fastest → most faithful)

### Path A — fit `RationalA` to a designed target curve (recommended; no dataset)
1. **Design the target static curve** `y = f(x)` for `x∈[-1,1]`: gentle, unity-slope
   near 0 (so low `SEQ_DRIVE` is subtle), even-harmonic-biased console warmth — a mild
   asymmetry (→ 2nd harmonic) plus a soft knee (→ 3rd). A good starting family:
   `f(x) = tanh(k·(x + β·x²)) / tanh(k)` with small `k` (≈1–2) and small bias `β`
   (≈0.05–0.15); tune by ear / harmonic ratios.
2. **Fit** `num`/`den` so `P(x)/Q(x) ≈ f(x)` by least squares over a dense `x` grid
   (small orders: `num` deg ~5, `den` ~3 mirrors the saturator). Enforce `f(0)=0`
   (`a0=0`) and unit slope at 0 for a clean bypass at low drive. A ~30-line Python/NumPy
   `lstsq` script is enough; deliverable = the target-curve spec + the fit script + the
   exported `num`/`den`.
3. **Verify** the fit generates the intended harmonic spectrum (sweep a sine at several
   levels; check 2nd/3rd dominate and 4th+ stay low), and that it's monotonic / stable.

### Path B — train a nablafx StaticRationalNonlinearity (if a real SSL reference exists)
Capture in/out pairs from a real SSL 9000 channel (sine sweeps + program at several
input levels, DC-through-Nyquist), then train exactly like the saturator (the nablafx
`StaticRationalNonlinearity` recipe) and export `num`/`den`. Same wiring as Path A.
Use this if you want the *measured* console curve rather than a designed one.

### Path C — model with memory (only if A/B sound flat)
If static colour proves lifeless (missing frequency-dependent grit / hysteresis), move
to a small stateful grey-box (e.g. a tiny TCN, like the bus-comp). Bigger effort, new
runtime block, non-trivial latency/CPU — defer unless clearly needed.

## Wiring the coefficients into the plugin
- Ship the fitted `num`/`den` either (a) embedded as constants and call
  `ch.ssl_eq.set_harmonic(num, den)` in `plugin_activate` (per channel), or (b) as a
  tiny `ssl_harmonic` sub-bundle (`num`, `den` in JSON) loaded next to the others in
  `create_plugin`, mirroring the saturator's bundle load.
- `SEQ_DRIVE` ("Colour", 0..1) already maps to `harmonic_mix` and mixes it in — no new
  control needed. Confirm `SEQ_DRIVE=0` stays bit-identical (it already gates `harm`).
- For the "colour" product framing, pair this with the musical calibration: run a
  gentle `SEQ_CAL` (partial, α<1) for the broad EQ voicing, and a little `SEQ_DRIVE`
  for harmonic character; the auto-EQ handles the residual transparently.

## Verification
- Unit: extend `tests/test_ssl_channel_eq.cpp` (it already covers the harmonic stage —
  `test_harmonic_stage`): assert `SEQ_DRIVE=0` is bypass-identical and that a loaded
  curve produces the expected 2nd/3rd-harmonic ratios via the existing Goertzel helper.
- Bench/A-B: `axon_bench` a sine + program with `SEQ_DRIVE` at 0 / low / high; compare
  spectra and loudness against bypass.
