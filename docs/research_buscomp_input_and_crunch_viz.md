# Research: Bus Comp "Input" trim (SSC_IN) verification + visualizing the "crunch"

Scope: static analysis of `native/clap/src/axon_plugin.cpp`, `native/clap/src/ort_session.cpp`,
`native/clap/ui/index.html`, `docs/neural_inference_ort.md`. No production code was modified.
A synthetic probe (`/tmp/crunch_probe.py`) validated the metric math.

---

## GOAL 1 — SSC_IN verdict: intentional and correct

**Verdict: YES — the level-invariance is intentional, by-design, and correctly implemented.**
At SSC_IN = 0 dB the stage is bit-identical to before. The audible behaviour the user
describes ("changes crunch/character, level barely moves = magic") is exactly what the code
is built to produce.

### The mechanism (reciprocal makeup)

The bus comp is a fixed-curve model whose behaviour depends only on *how hard it is driven*.
SSC_IN multiplies the signal feeding the model by a linear gain, and multiplies the model's
wet output by the **exact reciprocal** before the wet/dry blend:

- Param → linear trim:
  `axon_plugin.cpp:1206` — `s.ssl_comp_in_lin = std::pow(10.f, ssc_in_db / 20.f);`
- Read into the stage and reciprocal computed:
  `axon_plugin.cpp:1443` — `const float in_gain = amt.ssl_comp_in_lin;`
  `axon_plugin.cpp:1444` — `const float mk_gain = (in_gain != 0.f) ? (1.f / in_gain) : 1.f;`
- Input trim applied to the signal entering the hop accumulator (the model's input ring):
  `axon_plugin.cpp:1464-1469` — `accum[fill + i] = blk[i] * in_gain;` (else `std::copy_n` at 0 dB)
- Reciprocal makeup applied to the model **wet** output only, after popping it from the queue
  and **before** the blend:
  `axon_plugin.cpp:1532-1535` — `wet_a[i] *= mk_gain;`
- Blend uses the time-aligned dry, not current dry:
  `axon_plugin.cpp:1540-1541` — `blend_inplace_(wet_a.data(), dry_aligned.data(), 1.f - amt.ssl_comp_wet, ...)`

Because `in_gain` and `1/in_gain` are applied on the same audio, the **linear scale cancels
to floating-point precision** — the model's operating point moves (it compresses/saturates a
different amount) but the gross level does not. That delta in the nonlinear/dynamic action is
the "crunch."

Synthetic confirmation (`/tmp/crunch_probe.py`):
- With a *linear* (identity) stand-in model, `wet = identity(dry*g)/g - dry` differs by
  `<= 1.1e-16` for g = 0/+6/+12 dB — the cancellation is exact.
- With a *nonlinear* compressor stand-in, the gross level shifts only modestly while the
  residual (model contribution) grows substantially with drive — i.e. character changes far
  more than level. This is precisely the user's "magic."

Important nuance to record: the cancellation is *exact for the linear component only*. A real
compressor reduces RMS as it's driven harder, so the reciprocal does not perfectly restore
loudness — it restores the *linear scaling* exactly and leaves the compression-induced level
change. That residual loudness drift is small for normal trims and is what the in-code comment
calls "roughly level-neutral" (`axon_plugin.cpp:1434`, `:1529-1531`). So "level-invariant" is
accurate in practice but is not a literal loudness lock; it's an exact linear-gain cancellation.

### Dry path untouched / alignment preserved

- The dry reference is captured **before** any trim: `axon_plugin.cpp:1448`
  `std::copy_n(blk, kBlockSize, dry.data());` — neither `in_gain` nor `mk_gain` touches it
  (comment `:1460-1463`, `:1436`).
- Wet/dry sample alignment is preserved by the dry-delay ring of length
  `kSslHop - kBlockSize` (`:1430`, `:1501-1517`), so the blend mixes wet against the dry that
  is the *same audio in absolute time* — no hop-rate comb flutter. SSC_IN does not change this
  path at all.

### Bit-identical at 0 dB

At SSC_IN = 0 dB, `ssl_comp_in_lin == 1.0`, so `in_gain == 1.f` and `mk_gain == 1.f`. The
guards at `:1464` and `:1532` skip the multiply entirely and fall back to `std::copy_n`, so the
signal path is byte-for-byte what it was before the control existed. Confirmed.

### The automation-time-skew caveat (real, but small)

The model output trails its input by the hop/lookahead latency, so `in_gain` (applied to the
model *input*, `:1464`) and `mk_gain` (applied to the model *output*, `:1532`) act on the same
audio at **different host calls**. The in-code comment flags this (`:1438-1442`).

Magnitude, from the constants:
- `kBlockSize = 128`, `kSslHop = 1024` (`axon_plugin.cpp:81,94`).
- The wet output trails its input by `kSslHop - kBlockSize = 896` samples
  ≈ **18.7 ms at 48 kHz** (≈ 20.3 ms at 44.1 kHz). (The model's own rf=631 warmup is internal
  and does not add to the in→makeup skew; only the hop accumulation does.)

Consequence: for a **static** trim this is exactly correct (same constant on input and output).
During **fast automation** of the knob, for ~18.7 ms the output is briefly scaled by
`mk_gain(new)` while the audio in flight was driven by `in_gain(old)`, so a momentary level
bump of up to the size of the knob jump appears (e.g. a sudden +6 dB jump → up to ~+6 dB / one
mismatched-block transient until the pipeline flushes 896 samples). This is a sub-20 ms glitch,
acceptable for an operating-point control and consistent with the comment. Not a bug. If it
ever mattered, the fix would be to delay `in_gain` updates by the same 896 samples (or smooth
the knob) — not recommended unless users automate it aggressively.

### Edge cases / bugs

- **Divide-by-zero guard:** `mk_gain` guards `in_gain != 0.f` (`:1444`). `in_gain` is
  `10^(dB/20)`, which is never 0 for finite dB, and the param min is -24 dB
  (`10^(-1.2) ≈ 0.063`) per the injected spec `axon_plugin.cpp:2097`. So the guard is belt-and-
  suspenders; no real divide-by-zero path.
- **+12 dB extreme:** at SSC_IN = +12 dB, `in_gain ≈ 3.98`, `mk_gain ≈ 0.251`. The makeup
  *attenuates* the model output, so it does **not** amplify model output noise — if anything it
  reduces any model-quantization/inference noise floor by ~12 dB. At -24 dB, `mk_gain ≈ 15.8×`
  (+24 dB) *does* amplify whatever the model emits, including its noise floor and any DC/denormal
  the TCN produces, by +24 dB. That is the one direction that can raise the noise floor; for a
  bus-comp model fed a -24 dB-attenuated signal this is an unusual setting but worth knowing.
- **Clipping:** SSC_IN does not clip internally; the downstream TruePeakCeiling / limiter handle
  peaks. The reciprocal makeup brings the wet back to ~unity scale before the blend, so the
  stage does not by itself push levels into clipping at sane trims.
- **Denormals:** no new denormal risk from SSC_IN itself; the multiply by a normal constant
  can't create denormals from normal input. (Any denormal handling is the existing chain's job.)

**Bottom line:** intentional, correct, well-commented, guarded. The only caveats are (a) the
~18.7 ms automation skew (documented, minor) and (b) "level-invariant" = exact *linear* gain
cancellation, not a literal loudness lock (the comp's own RMS reduction still shows). No bug.

---

## GOAL 2 — Visualizing the "crunch"

### Why there is no "true" gain-reduction readout

The bus comp is a learned streaming TCN exported to ONNX. The ORT session produces a **single
output tensor, `audio_out`** — processed audio only (`ort_session.cpp:119-141`, run with
`"audio_out"` at `axon_plugin.cpp:1486-1488`; model contract in `docs/neural_inference_ort.md`:
trace_len 2048, rf 631, one audio output). There is **no internal gain-reduction signal** to
read, unlike the MelLimiter which computes explicit per-band gains. So any "how hard is it
working" meter must be **derived from input vs output** — a proxy. Confirmed.

### The key opportunity: the residual is already in hand

Inside the SslComp block, after the makeup, both signals exist in the same buffer scope, sample-
aligned, every block:
- `wet_a[]` — the model wet (post-makeup), `axon_plugin.cpp:1526-1535`
- `dry_aligned[]` — the time-aligned dry reference, `axon_plugin.cpp:1506-1517`

So the model's *contribution* is directly available as `wet_a[i] - dry_aligned[i]` **with zero
extra latency or buffering** — the hard part (time alignment) is already done for the blend.

### Recommended proxy metrics (ranked)

1. **Residual energy — RMS of (wet − dry_aligned), in dB. [TOP PICK]**
   A direct "how much is the model altering the signal" meter. No clean-fundamental assumption
   (unlike classic THD), trivial to compute, and it tracks SSC_IN beautifully: in the synthetic
   probe the residual RMS rose from -26 dB (0 dB drive) to -17 dB (+12 dB drive) while output
   level barely moved — exactly the invisible-on-the-output-meter behaviour we want to expose.
   This is the single most honest "crunch/activity" number.

2. **Crest-factor reduction — crest(dry_aligned) − crest(wet) over a window, in dB.**
   crest = peak/RMS. The drop is the most *intuitive* "compression amount" readout (dynamics
   being squashed) and is cheap (one peak + one RMS per signal). Caveat from the probe: when the
   stage is dominated by saturation rather than dynamics, crest reduction can plateau (it read
   ~5.6–6.0 dB across drives in the toy model) — it captures *compression*, not *saturation*.
   So pair it with (1): residual = total activity, crest-reduction = the compression share.

3. **Spectral character / THD-style harmonic proxy of the residual. [OPTIONAL, skip first]**
   You could FFT the residual to show *where* the crunch lives, or estimate added-harmonic
   energy. Honest call: not worth the complexity for a v1. THD-proper needs a known fundamental
   (program material has many), and the residual already answers "how much"; "where" is a nice-
   to-have. The plugin already has a spectrum analyzer (`spectrum.build_js`) if a spectral view
   is ever wanted — but a residual *level* + crest-reduction covers 90% of the value.

### Feasibility vs. the existing telemetry path

The limiter GR strip is the perfect template and shows the whole pattern is cheap and proven:

- **Audio-thread tap → atomics/locked snapshot:** limiter does
  `copy_display(...)` under a `try_lock` (`axon_plugin.cpp:1818-1827`) into `lim_levels/lim_gains`
  (`:721`); meters use plain atomics (`m_out_rms.store(...)`, `:1633-1640`).
- **Main-thread push to JS:** `plugin_on_main_thread` builds a JSON string and calls
  `axon_gui_eval_js(...)` at ~21 fps — see `axonMeters(...)` (`:1976-1987`) and the limiter block
  `axonLimiter({...})` (`:1990-2028`).
- **UI receiver + scrolling strip:** `window.axonLimiter` (`index.html:2024-2050`) feeds a ring
  buffer via `pushGrHistory` (`:1833-1837`, `GR_HIST_N=180`, `grHist`/`grBandHist` at `:1829-1832`)
  and `drawLimGR()` (`:1953-2022`) renders the scrolling GR strip into `#lim-gr-canvas`
  (`:449`, drawn from `:848` / `:2049`).

A crunch meter slots into this exact pipeline with a small DSP-side tap.

### What would need adding (concrete)

**DSP side (small tap — required):** in the SslComp block, right after the makeup/blend
(around `axon_plugin.cpp:1535`, using `wet_a[]` and `dry_aligned[]` before the copy at `:1542`):
accumulate per-block:
- `sum_resid2 += (wet_a[i]-dry_aligned[i])^2`, `sum_dry2`, `sum_wet2`
- `peak_dry = max(|dry_aligned[i]|)`, `peak_wet = max(|wet_a[i]|)`

Then compute residual RMS (dB), crest(dry)−crest(wet) (dB), and store into 2–3 atomics (mirror
`m_out_rms.store`, add `m_ssc_resid_db`, `m_ssc_crest_red_db` next to `:717`). Only do this when
SSC is active (`amt.ssl_comp_wet > 0`). Cost: a handful of FLOPs over 128 samples — negligible.
~20–30 lines.

**Telemetry side:** add the two/three fields to a small JSON push, e.g. a new
`axonBusComp({"resid":..,"crestRed":..,"active":..})` in `plugin_on_main_thread` (clone the
`axonMeters` block, `:1974-1988`). ~10 lines.

**UI side:** a `window.axonBusComp` receiver + a readout. Two options:
- *Cheapest:* two numeric readouts ("Crunch −X.X dB residual", "Comp −Y.Y dB crest") on the
  SslComp stage panel. Pure additive JS, ~20 lines.
- *Nicer:* a scrolling "Model Drive / Crunch" strip mirroring `drawLimGR` — copy the
  `grHist`/`pushGrHistory`/`drawLimGR` trio (`:1829-1837`, `:1953-2022`) into a `crunchHist` +
  `drawCrunch` and bind it to the SslComp stage view (gate like `selectedProc === 5` at `:2049`).
  ~60–80 lines, no new DSP beyond the tap.

### Ranked recommendation

| Rank | Deliverable | Effort | Value | Notes |
|------|-------------|--------|-------|-------|
| 1 | DSP tap (residual RMS + crest-reduction) + 2 numeric UI readouts | Low (~50 lines total, no new buffers) | High | Makes the invisible SSC_IN action legible; residual is the honest "magic meter" |
| 2 | Scrolling "Crunch" strip mirroring `drawLimGR` | Medium (+~70 UI lines) | High polish | Reuses proven ring-buffer/draw pattern; best UX once #1 exists |
| 3 | Residual spectrum / THD harmonic view | Higher | Marginal for v1 | Skip unless users ask "where" not just "how much" |

**Honest call:** worth building. It needs a *small* DSP tap (cannot be pure UI — the residual is
only available inside the audio-thread SslComp block), but the alignment work is already done and
the telemetry/strip infrastructure already exists. The residual-RMS meter in particular directly
visualizes the very thing the user finds "magic": the model character moving while the level
holds. Crest-reduction is a good intuitive companion but plateaus on saturation-dominant
settings, so present it alongside residual, not alone.
