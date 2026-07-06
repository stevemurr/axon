# Auto EQ: two controllers, two renderers, and the mode-collapse diagnosis we had to retract

Auto EQ is the stage that decides its own curve. Every 128-sample block, a controller listens to the program and emits 64 per-band gains; a renderer turns those gains into an actual filter and applies it. Nothing about that is exotic. What makes this module worth a deep dive is that it exists **twice on both axes** — two controllers (a per-class neural LSTM and a deterministic target-match cascade) and two renderers (an STFT minimum-phase mask and a zero-latency IIR filterbank) — all four combinations live behind two switches in the shipped plugin, and every cell of that 2×2 matrix earned its place through a measurement that could have killed it.

It is also the most instructive module in the codebase about being wrong in public. Three stories run through this doc:

1. **The A/B harness that refuted its own rationale.** The IIR renderer was built because "the STFT smears transients and makes musical noise." The strengthened harness showed neither claim held — the IIR shipped anyway, for latency and CPU, with the sonic claim explicitly retracted (§7).
2. **The mode-collapse diagnosis we had to retract.** For a month the working theory was "the LSTM mode-collapses to a class-mean curve, so we're paying 24% of process CPU for a static EQ." Contract-exact probing of the shipped 64-band models proved that conclusion was an artifact of probing a **dead contract** — the old 5-band PEQ models. The shipped controllers genuinely adapt (§8).
3. **The wobble debugging where the math was right.** When the deterministic engine shipped and users reported "extremely wobbly" EQ, the cascade algorithm — faithfully ported from the validated bench twin — was innocent. All three bugs were plumbing (§9).

Primary sources: `native/clap/src/spectral_mask_eq.hpp`, `native/clap/src/iir_filterbank_eq.hpp`, `native/clap/src/adaptive_eq.hpp`, `native/clap/src/adaptive_eq_targets.hpp`, and the `StageID::AutoEQ` case in `native/clap/src/axon_plugin.cpp:1563`. All line references are against `origin/main`.

---

## 1. The 2×2 matrix (plus a freeze button)

Three switches control the stage's topology (`weights/axon_bundle/axon_meta.json`):

| Control | Name | Values | Default | What it picks |
|---|---|---|---|---|
| `EQ_ENGINE` | AutoEQ Engine | 0 = Neural, 1 = Adaptive | **0 (Neural)** | which *controller* produces the 64 band gains |
| `EQ_RENDER` | AutoEQ Renderer | 0 = STFT mask, 1 = IIR bank | **1 (IIR)** | which *renderer* applies them |
| `EQ_FREEZE` | AutoEQ Freeze | 0 = Live, 1 = Hold | 0 (Live) | hold the last live-solved curve, skip all controller inference |

The defaults are resolved in `resolve_amount_` (`axon_plugin.cpp:1383-1385`) — note `eqrnd=1.f` at `:1384`: **the IIR renderer is the default**, a consequence of the latency/CPU verdict in §7. The neural engine remains the default controller because the retraction in §8 established it is doing real, program-following work.

The remaining user controls:

| ID | Name | Range | Default | Effect |
|---|---|---|---|---|
| `EQ` | Auto EQ | 0–1 | 1.0 | wet/dry blend (× `amount_mapping.auto_eq.wet_mix_max` = 1.0), applied by `blend_` at `axon_plugin.cpp:1742`. 0 = bypass (but see §11 — the controller still runs). |
| `CLS` | EQ Class | enum 0–4 | 4 | class selector: bass / drums / vocals / other / full_mix (`class_order` in axon_meta). Selects the LSTM model **and** the adaptive engine's empirical target curve (`:1578-1585`). |
| `EQR` | EQ Range | 0–1 | 1.0 | scales the predicted dB curve toward 0 (both renderers: `set_range_norm`). |
| `EQB` | EQ Boost | 0–1 | 1.0 | attenuates *boosts only* — STFT renderer only (`spectral_mask_eq.hpp:144`, applied `:175`). |
| `EQS` | EQ Speed | 10–500 ms | 100 | STFT renderer: bin-gain smoother tau (`:1729`). Adaptive engine: emitted-curve smoother via `set_response_ms(200 + 2·EQS)` → 0.22–1.2 s (`:1635`). |

Every one of these reads must use the literal `c.id=="XYZ"` spelling in `resolve_amount_` — `tests/test_control_contract.cpp` regex-extracts the read-set from the source and diffs it against the shipped `axon_meta.json` (the HARD RULE comment at `axon_plugin.cpp:1364-1372`). That contract test exists because the meta and the C++ read-set had silently drifted once before.

Where the stage sits: `StageID::AutoEQ = 1` (`axon_plugin.cpp:107`), and the default order (`kDefaultStageOrder{6,3,1,8,9,4,5}`, `:125`) is

```
BassMono → SslEq → AutoEQ → Reverb → Widener → SslComp → MelLimiter
```

SslEq deliberately precedes AutoEQ so its coupling assist bands can pre-EQ what the Auto EQ sees (`:711-714`). All stages are GUI-reorderable; `state_load` accepts only a permutation of the known stage set (`:1024-1048`).

---

## 2. The interchange contract: 64 numbers in [0, 1]

Everything in the matrix composes because both controllers emit, and both renderers consume, the same thing: `n_bands` sigmoid values in `[0,1]`, mapped linearly onto the trained gain span:

```
db[b] = min_gain_db + g[b] · (max_gain_db − min_gain_db)        g ∈ [0,1]
```

so `g = 0.5` is exactly 0 dB. The geometry comes from each class bundle's `plugin_meta.json` and is identical across all five classes (enforced at load — every class must declare `spectral_mask_eq` as `dsp_blocks[0]`, `axon_plugin.cpp:1140-1141`):

| Constant | Shipped value |
|---|---|
| `sample_rate` | 44100 |
| `block_size` | 128 (= `kBlockSize`, `axon_limits.hpp`) |
| `n_bands` / `num_control_params` | **64** |
| `n_fft` / `hop` (STFT renderer) | **4096 / 2048** (50% overlap) |
| `min_gain_db` / `max_gain_db` | **−18 / +18 dB** |
| `f_min` / `f_max` | 30 Hz / 22050 Hz |

The 64 bands are mel-spaced by the HTK formula (`mel = 2595·log10(1 + f/700)`): 66 equally-spaced mel points between `f_min` and `f_max`, each adjacent triple defining a triangular band. Both renderers and both controllers build centers from the shared `mel_scale.hpp` helpers so bands line up exactly.

The audio thread stages these params through fixed 64-element stack arrays (`kEqParamsStorage`, `axon_limits.hpp`); `plugin_activate` rejects any bundle declaring more (`axon_plugin.cpp:1150-1155`), pinned by `tests/test_autoeq_param_guard.cpp`.

One asymmetry worth knowing: analysis and rendering are decoupled by design. A controller is **analysis-only** — it observes input and reports a curve; it never touches the audio path. So controller-side analysis lag (a 2 s running spectrum, an LSTM's state) adds **zero output latency**; only the renderer's structure does (§5, §6).

---

## 3. Controller A: the per-class neural LSTM

Five ONNX models (bass/drums/vocals/other/full_mix), each a **single-timestep** LSTM: `receptive_field: 1`, hidden size 64, 2 layers; the whole 128-sample block enters as one input vector, passes a Reshape/Transpose frontend, two LSTM cells, a small MatMul head, and a Tile ×128 (only timestep 0 of the output is ever read — `run_controller` at `axon_plugin.cpp:562-566`). State tensors `root_h`/`root_c` `[2,2,64]` carry across blocks, which is why the model must be fed every block.

The per-block path (`axon_plugin.cpp:1638-1674`):

1. **Peak-hold normalization** (`:1648-1659`): training normalized peak per ~10 s segment; a per-block peak collapsed the LSTM's input distribution at runtime. So each channel keeps an attack-instant / ~500 ms-decay envelope (`kEnvTauSeconds = 0.5f`, `:1090`) and the block is scaled to ~0.5 peak before inference.
2. **One batched ORT call for both channels** — `run_controller(ctrl_l, ctrl_r, …)` then `swap_state()` (`:1666-1669`).
3. Class switches (`CLS`) zero the incoming session's LSTM state and the peak envelopes (`:1567-1573`) so hidden activations conditioned on a different signal class never leak.

### The batch-2 graph surgery (2026-07-05)

The models were exported batch-1, so stereo cost two ORT calls per block. Rather than retrain, the five ONNX graphs were resized **in place** to batch 2: `audio_in` `[1,1,128]→[2,1,128]`, state `[1,2,64]→[2,2,64]` — one Reshape constant had its batch hardcoded (`[1,1,128]`) and needed rewriting to `[0,1,128]` (0 = "copy from input"). `OrtMiniSession::run_controller` (`axon_plugin.cpp:526`) stacks L/R into a `[2,1,T]` tensor (batch element 0 = left/mono, 1 = right; mono feeds the same buffer twice and reads only element 0), and per-channel LSTM state lives in the batch dim. Sessions moved from per-`ChannelChain` to Plugin level (`:741`).

Verification bar: batched output equal to 2× batch-1 within **1.2e-7** over 2000 state-carried blocks, under both ORT 1.25 (Python) and the embedded 1.20 (C++); in-plugin neural render null **−131.4 dBFS** vs the pre-change build.

Payoff: less than hoped. `AutoEqOrtCtrl` went 2×53.3 → 1×92.2 µs/block; stage total 132.8 → 118.8 µs (−3.3% of process CPU). The explanation became the controller's defining perf fact: a single-step LSTM is ~100k MACs — **single-digit microseconds of arithmetic** — yet the call costs ~92 µs because ONNX Runtime spends it on per-node dispatch across ~20 graph nodes. Batching reduces Run calls, not nodes per call. Alloc-elimination measured only ×1.03. The graph is *dispatch-bound*, which is why the open plan (`docs/future/not-started/custom-lstm-controller-cell.md`, issue #16) is a hand-rolled C++ LSTM cell with weights extracted from the ONNX: estimated 10–25×, controller ~26% of process CPU → ~1–2%, and no audio-thread ORT allocations.

---

## 4. Controller B: the deterministic C1→C2 cascade

`nablafx::AdaptiveEqController` (`adaptive_eq.hpp:240`) is the productionized winner of a research-then-A/B campaign. The deep-research pass ranked four real-time adaptive-EQ paradigms for a mastering bus: (1) **deterministic target-spectrum match** — the backbone, inherently adaptive, cannot mode-collapse; (2) **dynamic resonance suppression** (soothe2-style), best layered on top of (1); (3) perceptual rebalancing (Gullfoss-style specific loudness), best as a *weight* on (1)'s error; (4) neural — best as a small residual on the solver, not standalone. Offline match-EQ paradigms (Ozone-style learn-then-apply) were excluded as non-streaming.

Candidates C1/C2/C3 and the cascade were implemented against a shared contract (`bench/sonic/eq_ctrl.hpp`: `IEqController` — `observe()` + `target()`, `:262-269`) with shared estimators so they stayed comparable, and scored by `bench/sonic/harness_eq_control.cpp` over a spectrally-distinct battery (brown/pink/white/bright noise, dark/bright music-like — `:78-88`) on seven metrics (`:1-10`): adaptivity (‖cross-material per-band curve std‖ — a collapsed controller scores ~0), activity, target match (level-invariant *shape* error, `:152-168`), crest delta, modulation, resonance reduction, loudness delta.

**The A/B outcome:** the C1→C2 cascade (`bench/sonic/eq_ctrl_cascade.hpp`) won decisively — `adapt = 36.9` (do-nothing baseline: 0), `match = 2.35` (baseline 5.83), `reso = 6.33`, `loud_d = −0.87`, transparent on transients. Two round-2 fixes made the numbers: the **energy-domain makeup** and the **anti-zipper trio** (below), which took C1's steady-tone modulation from **−37.9 to −60.3 dB**.

### The per-block math

One shared `RunningMelSpectrum` — an EWMA of windowed-FFT **mean** power per mel band, in dB — feeds both halves. Mean, not sum: dividing by Σ filter weights per band makes a flat input read as a flat mel-dB curve, so the estimate is directly comparable to a per-frequency target (`eq_ctrl.hpp:205-215`). The EWMA pole is `α = exp(−1 / (fs·τ / hop))` with **τ pinned at the validated 2 s** (`adaptive_eq.hpp:114`, `:244`).

Per `target_db()` call (`adaptive_eq.hpp:281-324`, mirrored from `eq_ctrl_cascade.hpp:10-22`):

```
MATCH (C1): raw[b] = target(center[b]) − in_db[b]
            sm     = gaussian_smooth_db(raw, σ_match);  zero-mean(sm)
            per band: !band_active → 0
                      else soft_deadband(sm − mean, tolerance_db(f))
CUT (C2):   base   = gaussian_smooth_db(in_db, σ_cut)          # local floor
            excess = in_db[b] − base[b]
            cut    = excess > thresh ? −depth·(excess−thresh)^sharp : 0
            clamp to [min_gain_db, 0];  per-band attack/release smoothing
SUM:        g[b]   = sm[b] + cut[b]
SMOOTH:     out_db[b] = α·out_db[b] + (1−α)·g[b]                # τ ≈ 0.40 s
MAKEUP:     g[b]   = clamp(out_db[b] + makeup_db(out_db, in_db), min..max)
```

The C1 update law is the Mockenhaupt/Nercessian (DAFx 2024) recipe; the target is a *tolerance band*, not a line (`tolerance_db`: ±1.5 dB mids, ±2.5 dB sub and air, `adaptive_eq.hpp:62-65`) — tonal-balance references are ranges (Ma/Reiss/Black, iZotope TBC).

Three details carry the sonic quality:

- **Energy-domain makeup** (`adaptive_eq.hpp:85-94`). A zero-mean-in-dB curve is *not* loudness-neutral, because dB↔power is nonlinear — boosting high-energy bands raises loudness even if the dB mean is zero. The makeup nets it out in the power domain using the controller's own spectrum estimate (no output measurement): `E_in = Σ 10^(spec_b/10)`, `E_out = Σ 10^(spec_b/10)·10^(g_b/10)`, trim `= −10·log10(E_out/E_in)` added to every band. In the A/B this fixed C1 from +3.8 dB of loudness drift to +0.08, and C3 from +14 to +0.6. Ordering matters: the curve is **smoothed first, then made up** (`:313-318`) — computing makeup on the pre-smoothed curve made it pump with C2's frame-to-frame jitter (the cascade's residual modulation source in round 1).
- **Soft deadband** (`:97-101`): shrink toward zero by the tolerance instead of hard-gating, so a band dithering across the tolerance edge produces a small continuous change, not a 0↔±tol zipper.
- **Per-band noise gate** (`:104-108`): don't EQ bands more than 45 dB below the loudest band — boosting the floor was the main source of steady-tone zipper and over-boost.

C2's cut state has per-band attack/release poles, scaled faster at HF (hf_scale 1→0.45 log-spaced, `:266-275`; attack 50 ms toward more cut, release 180 ms) — HF resonances flare and decay faster.

### Data-driven target curves

The original target was analytic (~5 dB/oct decay 100 Hz→4 kHz — still present as `adaptive_eq_detail::target_db`, `adaptive_eq.hpp:53-59`, and in the bench twin). Production now uses **empirical corpus curves**: `scripts/gen_target_curves.py` reads per-class median long-term log-mel spectra (`weights/auto_eq_refs/*.npz`, from `extract_class_targets.py` over 100 tracks/class) and generates `adaptive_eq_targets.hpp` — five 64-point curves plus a log-frequency-interpolating `target_curve_db()` lookup (`adaptive_eq_targets.hpp:142-157`).

The generator's critical fix is a **convention reconciliation**: the extractor computes `power @ fb.T` — a *sum* over the triangular mel filters with no area normalization, which rises toward HF for white input (wider bands sum more bins) — while the runtime `RunningMelSpectrum` uses the per-band *mean* (density). The generator converts sum→density by subtracting `10·log10(Σ filter weights)` per band, reconstructing the exact filterbank from the stored `(n_fft, n_mels, sample_rate)`, then re-zero-means (shape is all that matters; the makeup handles level) — `gen_target_curves.py:40-70`. Without this, the controller would have chased a phantom HF tilt of several dB.

The active curve follows `CLS` (`axon_plugin.cpp:1578-1585` → `set_target_curve`); unknown names fall back to `full_mix` (index 0).

### Runtime shape

The controller is **linked-stereo by design**: ONE Plugin-level instance (`axon_plugin.cpp:733`) observes the L+R mono sum once per block (`:1627-1636`) and its single curve is rendered on every channel — the reason is a measured bug, not taste (§9, bug B). `RunningMelSpectrum` is movable (transfers the `FFTSetup`, `adaptive_eq.hpp:142-146`) so owning objects can live in vectors. `target_bands()` (`:328-337`) maps dB → the `[0,1]` contract; `latency_samples()` returns 0 (`:339`) — analysis-only. Unit coverage: `tests/test_adaptive_eq.cpp` asserts stability/bounds, adaptivity (‖dark−bright‖ > 3 dB — "clearly not collapsed"), correct tonal direction, determinism, and the depth-0 bypass (all bands = 0.5).

Because the smoothing widths were tuned at the 24-band bench layout, `reset()` rescales them to **frequency**, not band count: `σ` is multiplied by this layout's bands-per-octave over the bench reference (`24 / log2(18000/40)` ≈ 2.72 bpo), so the 64-band production layout (≈ 6.72 bpo) gets σ ≈ 7.4 / 9.9 instead of 3 / 4 (`adaptive_eq.hpp:248-257`). That's §9's bug C, fixed at the root.

---

## 5. Renderer A: the STFT minimum-phase mask

`SpectralMaskEq` (`spectral_mask_eq.hpp`) is the original renderer, a C++ mirror of the Python training-side `SpectralMaskEQ` processor (same n_fft, hop, HTK mel edges, Hann analysis+synthesis windows) with two additions: minimum-phase application and two-stage mask smoothing. It is pure DSP — no CLAP/ORT/variant dependencies — so it unit-tests standalone.

### Streaming STFT/OLA

`process(in, out, n)` accepts arbitrary `n` (`:217-235`): samples accumulate into an `n_fft` ring; every `hop` samples, `run_frame_()` (`:254`) windows the ring oldest-first (two contiguous `vDSP_vmul` segments — no per-sample modulo), forward-FFTs (`vDSP_fft_zrip`, radix-2 split-complex), applies the mask, inverse-FFTs, re-windows (Hann² envelope) and overlap-adds into an output ring; each input sample pulls one finished output sample.

Because analysis *and* synthesis windows are applied, the overlapping windows don't sum to a constant. Normalization matches `torch.istft`: the OLA accumulates `window²` in a parallel ring and divides each output sample by its accumulated window² (`OlaAccumulator::pull_sample`, shared in `stft_common.hpp`), robust even as the COLA sum varies 0.5–1.0 over the hop cycle. The forward+inverse vDSP round-trip scale of `2·n_fft` is folded into `ola_scale_` (`:129-135`).

**The OLA ring clamp** is a defended regression: `add_frame` clamps `avail` to ring capacity (`clamp_avail=true`, `:297-307`). The old code did an unconditional `avail += hop`; if `process()` ever fell behind frame generation, the read pointer chased the writer into cells the OLA `vDSP_vadd` was mid-writing. `tests/test_spectral_mask_eq.cpp` white-box-drives the private `run_frame_()` under a 1000-frame stall (`#define private public`) and asserts `ola_.avail ≤ n_fft + hop` every frame, plus behavioral bounded-output guards under varied block sizes (1, 7, 333, 3000, 4096 …).

### From 64 bands to 2049 bins (`set_params`, `:157-214`)

1. **Sigmoid → per-band dB**, with Range as a scale around 0 dB and Boost attenuating positive dB only (`:169-176`).
2. **Band → bin in the dB domain**: vectorized `vDSP_vsma` accumulate over the `[n_bands × n_freq]` triangular matrix, divided by `bin_norm_[k]` (Σ band weights at bin k) (`:180-190`).
3. **1/6-octave Gaussian frequency smoothing, in dB** (`:196-205`). The kernel is precomputed per output bin with σ proportional to frequency — LF bins get a ~1-bin kernel, HF bins a wide one (capped at half-width 32), stored as a ragged array (`build_freq_smoothing_kernel_`, `:386-434`). Smoothing in dB is a geometric mean in linear — perceptually well-behaved when adjacent bins differ by many dB. This kills band-edge interpolation wiggle and the partial-tone jitter that reads as "musical noise" on tonal material.
4. **Time smoothing** (`:209-213`): a one-pole from `bin_gain_target_` toward `bin_gain_`, stepped once per `set_params` tick. Two clocks meet here: `set_params` runs every 128 samples (~2.9 ms), the FFT consumes the mask every 2048 samples (~46 ms). Without the smoother, the ~21 Hz frame-rate stepping of the mask shows up as graininess on kick/bass. The EQS knob sets this tau (`set_speed_tau_ms`, `:150-153`; recompute only on change, `:378-384`).

### Minimum-phase application — and why latency = n_fft

A frequency-domain EQ must choose the filter's phase. Applying the real magnitude mask directly gives a zero-phase filter with a symmetric impulse response — which **pre-rings**: an HF cut smears energy backwards into the silence before a transient, audibly softening kicks and cymbals. That perception ("loss of top-end energy" on transients) was the dominant complaint against the naive mask.

Instead, each frame builds a minimum-phase filter with identical |H| via the real-cepstrum recipe (Oppenheim & Schafer; `compute_min_phase_`, `:323-376`):

1. `log|H[k]|`, floored at 1e-7 (−140 dB)
2. real cepstrum `c[n] = IDFT{log|H|}`
3. fold the anti-causal half onto the causal half: `w = [1, 2×(N/2−1), 1, 0…]`
4. `log H_mp = DFT{c_min}`
5. `H_mp = exp(·)` — vectorized with vForce `vvexpf/vvcosf/vvsinf` over 2047 bins (`:365-375`); DC and Nyquist come out purely real and are handled as scalar special cases (`:276-277`, `:361-364`). The complex multiply is `vDSP_zvmul` (`:279-285`).

This costs two extra FFTs plus the vForce transcendentals per frame — the price of causal application, paid every `hop` samples.

The latency consequence is subtle enough that the header spells it out (`:16-20`): a zero-phase mask could report `n_fft − hop` because its symmetric IR pre-rings into the leading zeros; the min-phase filter is causal, so the first meaningful OLA output sits at ring position `hop`, consumed at output sample `2·hop − 1 ≈ n_fft`. **`latency_samples()` returns the honest `n_fft`** (`:249`) — 4096 samples ≈ 93 ms at 44.1 kHz with the shipped geometry. (The `meta.hpp` struct comment that used to say `n_fft − hop` has been fixed — `meta.hpp:39` now agrees with the code.)

After `reset()`, the audio path allocates nothing; `set_params`/`process` are lock- and allocation-free.

---

## 6. Renderer B: the zero-latency IIR filterbank

`IirFilterbankEq` (`iir_filterbank_eq.hpp`) consumes the identical `[0,1]` contract but realizes the curve as a cascade of N biquads — RBJ peaking bells at the interior mel centers, with the **lowest and highest bands as shelves** (`filter_for_`, `:164-167`): peaking bells roll off beyond the outermost centers, so a tilt/shelf target would undershoot there — the dominant magnitude-match error before the shelves. Per-band Q comes from neighbor spacing (bell spans roughly to its neighbors, clamped 0.5–8, `:44-50`).

The non-obvious piece is the **interaction-matrix solve**. Overlapping bells add in dB in a cascade, so setting each bell's gain to the target at its center overshoots wherever bells overlap. At `reset()` the module measures `A[i][j]` = dB response at center *i* of a unit-gain filter *j* (using an analytic `|H(e^jω)|` probe on the biquad coefficients, `:113-122`) and inverts it once by Gauss-Jordan with partial pivoting (`:190-213`, off the audio thread). At runtime, each `set_params` solves

```
bell_gains = A⁻¹ · smoothed_target_curve        (one nb×nb mat-vec per block)
```

so the **summed** cascade response hits the per-band targets (`:81-86`), then refreshes all coefficients. Targets are time-smoothed by a fixed ~25 ms one-pole before the solve (`:59-61`, `:79-80`) — this renderer does not listen to EQS.

Zero latency (`:98`), in-place-safe mono `process()` — 64 serial double-precision biquads per sample (`:90-96`); the plugin runs one instance per channel per class. `magnitude_db(f)` (`:102-107`) sums the bells' analytic responses for the UI overlay and tests. `tests/test_iir_filterbank_eq.cpp` pins flat-is-unity (<0.1 dB worst, RMS ratio ~1), magnitude match on a known tilt (RMS < 0.5 dB at interior centers), and bounded output under ±12 dB slams.

Everything is header-only, `<cmath>`-portable (no Accelerate) — one reason this renderer matters for the cross-platform port.

---

## 7. The A/B doctrine — and the refuted clarity claim

`native/clap/docs/sonic_harness_spec.md` (now folded into this section; git history keeps the original) codified the rule that governs all sonic changes here: **match the effect, then compare the artifacts.** A processor that does less trivially distorts less, so `bench/sonic/harness_auto_eq.cpp` drives both renderers with the *same* fixed target curves (gentle tilt, a narrow −6 dB notch at 3 kHz — the worst case for IIR Q-match — and flat) through one measurement path, at the bench layout (24 bands, 2048/512, ±12 dB; the production layout is 64/4096/2048/±18 — shares, not absolutes, transfer). Renderer changes are QUALITY changes: proven with A/B numbers, never null-tested.

Measured 2026-06-07 (Release -O3, Apple Silicon):

| Metric | STFT mask | IIR bank | Verdict |
|---|---|---|---|
| Magnitude match, tilt | 0.58 dB RMS / 2.41 max | **0.43 / lower max** | IIR wins |
| Magnitude match, notch | **0.13 RMS / 0.64 max** | 0.21 / 1.58 | STFT tighter on narrow notches (per-bin mask) — IIR still under the 0.5 bar |
| Latency | 2047 meas / 2048 rep (46 ms at bench n_fft) | **~0** | IIR wins |
| CPU | 240 ns/sample | **55 ns/sample (~4.4×)** | IIR wins |
| Flat-mask null | −138.5 dBFS | **−240 dBFS (exact)** | IIR wins |

And the two metrics the IIR was *built for*:

- **Transient smearing** (impulse spread under an active curve): the STFT is actually **more** compact — tilt RMS-duration 0.065 ms / −40 dB tail 0.48 ms vs the IIR's 0.096 ms / 1.07 ms. Neither pre-rings. The min-phase STFT concentrates energy compactly (exactly why min-phase was chosen); the IIR's resonant bells ring slightly more.
- **Musical noise** (steady 1 kHz, whole curve swept flat↔tilt at 1/3/8 Hz; `moving_mask_spur`, `harness_auto_eq.cpp:183-199`): a wash — identical spurious sidebands (≈ −45.7 dB, Δ ≤ 0.2 dB at every rate). Both smooth at block rate, and a smooth-mask EQ (unlike a spectral gate) has no musical-noise mechanism to begin with.

**Corrected verdict, kept on the record:** the IIR filterbank is a **latency + CPU** win with exact flat reconstruction, *not* a clarity improvement. The original sonic rationale was false, and the harness existed precisely to catch that false "better." The IIR became the default renderer (`EQ_RENDER` default 1.0, `axon_plugin.cpp:1384`) on the strength of the honest wins — 46→0 ms of PDC and ~4.4× renderer CPU — with the sonic claim withdrawn.

---

## 8. The mode-collapse diagnosis we had to retract

For most of June 2026 the module's strategic narrative was: *the LSTM controller mode-collapses to a class-mean curve*. It motivated the deep-research campaign, the deterministic cascade, and a tempting cost argument — "we're paying 24% of process CPU for what is effectively a static per-class EQ curve; bake it into a constant."

The diagnosis had real evidence behind it — for a different model. The June probes measured the **old 5-band ±9 dB PEQ controller** (a 15-parameter contract). The training-side theory also checks out: nets trained on an L1/L2 *parameter* loss collapse toward the parameter mean (Nercessian DAFx 2020: the param-only net had the best parameter loss and the worst spectral match, 1.495 vs 0.078); the cure is training through differentiable DSP on a multi-resolution-STFT *audio* loss. All of that remains true and is why the deterministic backbone was still the right thing to build.

But the shipped models are the 64-band spectral-mask generation, and the probe had never been updated. When the "bake it to a constant" idea came up for real (2026-07-05), a **contract-exact** re-measurement was run first — replicating the plugin runtime bit-for-bit: peak-hold envelope preprocessing with the exact `kEnvTauSeconds = 0.5` decay, state-carried block-by-block inference, timestep-0 param read, params→dB via each class meta's gain span. The result, on the live 64-band models:

- **Within-track drift:** ~1.2 dB std / 8 dB peak-to-peak in the settled curve — surviving 500 ms smoothing, so it is not frame jitter.
- **Spectral, not level-driven:** the movement persists with the input envelope artificially frozen, so it is not the normalizer leaking level.
- **Across-material spread:** 13–16 dB between spectrally distinct programs.
- **State-carry matters:** 1.9 dB mean difference vs state-reset inference — the recurrence is load-bearing.

Conclusion, recorded in exactly these terms: **freezing the per-class curve would be dishonest** — it would silently remove real program-following EQ movement, plausibly the very behavior users like. The "paying 24% CPU for a static curve" framing is retracted; the legitimate CPU path for the neural engine is the dispatch-bound fix (custom cell, issue #16), not deleting the adaptation.

The measurement is now repeatable: `scripts/probe_auto_eq_adaptivity.py` was rewritten for the 64-band contract (commit 593f61e). It probes the shipped ONNX bundles directly (shape-driven, handling both batch-1 fresh exports and the batch-2 shipped models), runs a material battery (pink/dark/bright/quiet/loud/non-stationary colored noise, optional WAV), and issues a verdict on across-material spread of the settled curve: **ADAPTIVE ≥ 3 dB, WEAK 1–3 dB, COLLAPSED < 1 dB**, exiting 1 on COLLAPSED — a CI-able tripwire against future re-collapse (e.g. after a retrain). A `--run-dir` mode probes hydra training runs pre-export on the training host.

Two doc-hygiene notes from this episode. First: the *retraction is not yet fully propagated into code comments* — `adaptive_eq.hpp:3-4` still introduces the cascade as a replacement for "the LSTM Auto-EQ controller that mode-collapses to the class-mean curve." That sentence is stale; the probe header (`probe_auto_eq_adaptivity.py:4-7`) carries the corrected history. Second, the general lesson: **a diagnosis is only as current as the contract it probed.** The probe measured something real and the conclusion transferred silently to a model generation it had never touched.

---

## 9. The wobble bugs: the math was right, the plumbing wasn't

When the adaptive engine shipped behind `EQ_ENGINE`, the owner reported it "extremely wobbly." The instinctive suspect — the cascade algorithm — was checked first and exonerated: `src/adaptive_eq.hpp` faithfully reproduced the validated bench twin, all round-2 fixes present. The investigation (driving the controller with instrumented torture signals and measuring curve motion in dB/s) found **three plumbing bugs**, all shipped in commit 84e135b (2026-07-05):

**Bug A (primary): the EQS knob was coupled to the spectrum estimator.** The old `set_response_ms` set *both* the running-spectrum EWMA (tau = EQS×3, clamped ≥ 50 ms) and the output smoother. The A/B had validated a **fixed 2 s** spectrum average with a 0.40 s output smoother; at the default EQS=100 the spectrum tau became 300 ms — 4–7× too fast, re-solving the tonal target on every chord change — and the validated 2 s was unreachable from the UI (max EQS=500 → 1.5 s). Measured: 46.9 dB/s mean curve motion at defaults vs ~6.6 dB/s with the 2 s tau (**~7×**), peak motion 1108 → ~127. The fix pins the spectrum EWMA at 2 s inside the controller (`spec_.reset` default; the comment at `adaptive_eq.hpp:352-357` documents the incident) and remaps EQS to the output smoother only: `set_response_ms(200 + 2·EQS)` → 0.22–1.2 s, with the default EQS=100 landing exactly on the validated 0.40 s (`axon_plugin.cpp:1635`).

**Bug B: independent per-channel controllers.** L and R each ran their own cascade (the bench had been mono). On decorrelated material the two solves drift apart — measured **1.04 dB/band peak L/R divergence** at defaults — which the ear reads as stereo-image wobble. Fix: ONE Plugin-level instance observing the 0.5·(L+R) mono sum once per block, its single curve rendered on both channels (`axon_plugin.cpp:733`, `:1608-1636`). Divergence is now zero by construction, and the cost halved as a side effect (adaptive AutoEQ 84.8 → 66.5 µs/block). Note the contrast with the *neural* mono-sum proposal, which the owner **rejected** (perf ranking #3) — for the LSTM, mono-summing changes shipped behavior on stereo-asymmetric material; for the adaptive engine, linked stereo *is* the validated behavior and per-channel was the regression.

**Bug C: band-count-constant smoothing sigmas.** `match_sigma_=3` / `cut_sigma_=4` were tuned in *bands* at the 24-band bench layout; production is 64 bands over a wider range, so the same numbers smoothed a ~2.5× narrower spectral span — C2 chased sharper "resonances," measured ~4.5× peak slew. Fix: scale by bands-per-octave against the bench reference (§4), making the smoothing constant in frequency (production σ ≈ 7.4/9.9; identity at the bench/test layout, so the A/B numbers still bind).

Plus one responsiveness fix: **EWMA warm-up**. From reset, a plain 2 s EWMA drags its full tau before the curve means anything ("takes a second to solve"). `RunningMelSpectrum::frame_` now uses `a = min(α, n/(n+1))` (`adaptive_eq.hpp:183-187`) — the cumulative mean of all frames so far until the growing window reaches the EWMA tau, converging within a few frames, then the steady-state pole takes over.

Post-fix measurements: torture signal 46.9 → ~6.6–7 dB/s mean curve motion; steady material 0.1–0.3 dB/s; sustained pure-tone curve span 14 → ~0.1 dB. The **neural path was verified byte-identical** through the whole rewiring — the refactor moved sessions and controllers around but provably touched nothing the LSTM path computes.

The moral matches the retraction's: the validated algorithm survived contact with production; every failure was in the seams — a knob wired to the wrong time constant, a topology mismatch (per-channel vs linked), and constants that didn't survive a layout change.

---

## 10. The stage, block by block

The `StageID::AutoEQ` case (`axon_plugin.cpp:1563-1745`) per 128-sample flush:

1. **Class switch** (`:1567-1573`): if `CLS` changed, swap the active class, zero the new session's LSTM state and both peak envelopes.
2. **Freeze falling edge** (`:1593-1607`): mirroring class-switch semantics — reset LSTM state, peak envelopes, and the adaptive controller (whose warm-up mean re-converges in a few frames).
3. **Produce the curve** — one of three paths:
   - **Freeze** (`:1616-1625`): render the held `autoeq_held_l/r` curves; skip *all* controller inference. Held curves initialize to 0.5 (flat) and hold whatever was live when the toggle engaged; they are deliberately **not persisted** in session state (a reload re-solves).
   - **Adaptive** (`:1626-1637`): mono-sum → `set_target_curve(CLS)` → `set_response_ms` → `observe` → `target_bands(…, depth=1)` (Range is the renderer's job).
   - **Neural** (`:1638-1674`): per-channel peak-hold normalize → one batched `run_controller` → `swap_state`; sub-timed as `AutoEqOrtCtrl` when `AXON_STAGE_TIMING` is on.
4. **Remember the live curve** so engaging Freeze holds it (`:1676-1680`).
5. **Render per channel** (`:1681-1743`): `EQ_RENDER` picks the per-class IIR instance (`set_range_norm` + `set_params` + `process`) or the STFT instance (plus `set_boost_scale`, `set_speed_tau_ms`). Per-class *renderer* instances exist so mask-smoother state doesn't bleed across class switches (`:597-604`).
6. **Display decimation** (`:1697-1704`): channel 0 evaluates the 5-point overlay (at the historical PEQ centers 1010/110/1100/7000/10000 Hz) and the 50-point log-spaced bin curve for the GUI — but only every 8th flush (`autoeq_disp_tick & 7`), because the spectrum UI consumes the overlay only every 16th flush; evaluating 55 `magnitude_db` points across 64 biquads every block discarded 15/16 results unseen. Audio path untouched; measured AutoEQ 150.8 → 132.6 µs/block.
7. **Wet/dry blend** by `EQ` (`:1742`).

**Latency / PDC** (`compute_latency_`, `:941-979`): base is `kBlockSize` (the input accumulator) + true-peak ceiling lookahead; the STFT renderer adds `n_fft` = 4096 **only when** EQ wet > 0 *and* `EQ_RENDER` selects the mask (`:959-970`). The recompute fires on every param change and notifies the host, so toggling `EQ_RENDER` re-PDCs exactly like toggling EQ on/off — no internal alignment delay line; the plugin's output-slaved bypass FIFO stays aligned automatically.

**Downstream consumer — the SSL EQ coupling.** The main thread fits the SslEq's assist bands to the Auto-EQ display curve (`ssl_split` × `spectrum.mt_eq_bins`, `:2234`) on a `SEQ_CAL` edge and publishes via seqlock; the SslEq stage ramps toward them (`:1535-1556`). This is why SslEq sits *before* AutoEQ in the default order: the assist bands absorb part of the correction so the Auto EQ has less to do. That subsystem has its own doc (`docs/ssl_eq_coupling.md`).

---

## 11. Performance: where the microseconds went

From `native/clap/docs/perf_stage_ranking.md` (2026-07-05, `AXON_STAGE_TIMING` instrumented build, buf=128; shares are what transfer):

- **AutoEQ: 33.5% of process CPU** — second only to the SslComp TCN (58.5%). The ungated LSTM controller alone was **24.0%** (`AutoEqOrtCtrl`, 137,800 calls, mean 54 µs before batching).
- Shipped from that ranking: display decimation (150.8 → 132.6 µs/block, bit-identical) and the batch-2 controller (132.8 → 118.8 µs). The **mono-sum LSTM controller was rejected by the owner** (audible behavior change on stereo-asymmetric material) — and the subsequent retraction (§8) validated the caution: the per-channel curves differ because the model genuinely responds to per-channel content.
- **The engine premium, measured:** with `EQ_ENGINE=1`, AutoEQ measures **84.8 µs/block vs 132.6 neural** (pre-batch-2/pre-linking baselines) — the LSTM premium was ~48 µs ≈ **12% of total process CPU**. After the 2026-07-05 work the standings are roughly: neural ~118.8, adaptive (linked) ~66.5, **frozen 27.5** µs/block (`EQ_FREEZE` skips all inference; 119 → 27.5 measured at ship time, with the freeze-off render verified byte-identical to the pre-feature build).
- Still open in the ranking: the EQ=0 hard bypass gate (#8 — the controller runs even at wet 0; `blend_` just discards the wet), IIR L/R interleave (#9 — ×1.52 on the renderer, verified bit-identical), IoBinding for RT-safety (#11), and the two big LSTM options (#13 decimating re-export — retrain risk; #16 custom cell — the preferred path).

---

## 12. Ill-conditioning and the validation policy

Auto EQ (STFT renderer) is one of the plugin's two numerically ill-conditioned stages, and it shapes how *any* change here may be validated:

- The min-phase reconstruction (log → cepstrum → fold → exp) feeding the recursive per-block gain smoother amplifies a **single 1-ULP perturbation to ~−77..−89 dBFS** once the mask is *moving*. Proven cleanly: replacing a scalar divide with the bit-identical `vDSP_vdiv` nulls to exactly 0.0 with a **static** mask but −89 dBFS with a moving one. The module itself is deterministic (same binary vs itself = 0.0).
- On top of that sits a **standing same-binary ORT nondeterminism** at −86..−99 dBFS (`docs/future/not-started/ort_render_nondeterminism.md`, issue #19): identical renders through one freshly built binary occasionally differ at those levels and resolve to byte-identical on retry, despite single-threaded sequential ORT sessions. Prime suspect: allocation-address/alignment-dependent SIMD kernel paths in ORT/MLAS, whose 1-ULP wobbles the ill-conditioned stages then amplify.

Policy, already in force: two-binary null tests cannot validate refactors of this stage at the standard −120 dBFS bar. Use steady-state-only nulls with an inaudible tolerance, or algebraic/index-equivalence proofs; retry (2–3×) any sub−85 dBFS mismatch on an ORT-involving param set before blaming a change. All of this is inaudible (below the 16-bit floor, limiter downstream) — it matters for *verification methodology*, not sound. The IIR renderer, notably, has none of these problems (exact flat reconstruction at −240 dBFS).

Related benchmark trap: `axon_bench` does not reset plugin state between `--iters`, so only compare renders with identical `--iters/--warmup` (a 5-vs-1-iter compare shows −4.9 dBFS of pure state divergence).

---

## 13. Honest limitations and open ideas

- **Knob coverage is uneven across the matrix.** `EQB` (boost-only attenuation) exists only on the STFT renderer; the IIR bank ignores it. On the IIR renderer `EQS` does nothing in the *neural* engine (the IIR's own smoother is fixed at ~25 ms) and only shapes the adaptive engine's output smoother. With the shipped defaults (Neural + IIR), EQB and EQS are effectively inert — worth either wiring or greying out in the UI.
- **IIR narrow-notch match** is the renderer's known weak spot (0.21 RMS / 1.58 max vs STFT's 0.13/0.64 on the −6 dB @ 3 kHz bench notch). Fine for broad mastering moves; a surgical controller curve is realized slightly softer.
- **The cascade still modulates a sustained pure tone** (bench modul −35 dB vs the −61 dB do-nothing floor): C2 correctly identifies a lone tone as "a resonance" and cuts it. Inherent to soothe-style dynamic suppression; tunable via slower C2 attack or higher `thresh_db_`.
- **HF breathing follow-up** (`docs/future/not-started/adaptive-eq-hf-band-tau.md`, issue #15): HF band energy is burstier, so equal-variance estimation wants *longer* averaging in the top octaves — a per-band spectrum tau (2 s mids → ~4–6 s above ~6 kHz), or robust (median/percentile) band statistics. Conditional: only if listening still finds HF undulation post-wobble-fixes. Lookahead was evaluated and rejected — centering a 2 s average costs ~1 s of real latency.
- **Neural controller CPU** (`custom-lstm-controller-cell.md`, issue #16): dispatch-bound; hand-rolled cell is the 10–25× fix. Acceptance bar already defined (≤1e-6 vs ORT over 2000 state-carried blocks, IDENTICAL-class eval null).
- **Stale comment debt:** `adaptive_eq.hpp:3-4` still asserts the retracted mode-collapse claim (§8); the top-of-file chain comment at `axon_plugin.cpp:3-9` still describes the LA-2A-era chain.
- **Target curves are stem-class, not genre.** The empirical curves come from MUSDB-style stem classes; genre curves are a *data* problem (need a genre-labeled mastered corpus — deriving a curve is just averaging; inference only enters for auto-selecting one, clustering, or a reference-track mode).
- **The adaptive engine legitimately sounds different from the neural one** even after the wobble fixes: it adapts continuously toward steeper empirical targets, while the LSTM's movement is smaller-amplitude around its learned prior. `EQ_ENGINE` is a genuine A/B the user can hear, not two routes to one sound.
- **Bench twins drift by design.** `bench/sonic/eq_ctrl*.hpp` retain the analytic target and 24-band layout as the frozen A/B twin of the production controller; `harness_auto_eq.cpp` likewise runs the bench geometry. Production truth lives in `src/adaptive_eq.hpp` + the generated `adaptive_eq_targets.hpp`.

---

## 14. Supersedes and cross-references

This dive replaces:

- **`docs/auto_eq_spectral_mask.md`** (2026-06-05) — its STFT/mel/min-phase tutorial content is carried into §2 and §5 with corrected anchors; everything else had drifted: it documented one cell of the matrix (Neural+STFT) with no `EQ_ENGINE`/`EQ_RENDER`/`EQ_FREEZE`, a stale chain diagram (Saturator-era order; the Saturator is now dormant and the default order is BassMono→SslEq→AutoEQ→…), the old per-channel/batch-1 controller wiring, the pre-`OlaAccumulator` ring internals, and a "stale `meta.hpp:39` comment" warning that has since been fixed at the source.
- **`native/clap/docs/sonic_harness_spec.md`** — the match-the-effect doctrine, metric/threshold table, IIR-vs-STFT numbers, and the refuted-clarity verdict are preserved in full in §7 (the harness itself, `bench/sonic/harness_auto_eq.cpp`, remains the executable spec).

Still-live related docs: `native/clap/docs/perf_stage_ranking.md` (cross-stage CPU ranking; §11 summarizes only its Auto-EQ rows), `docs/ssl_eq_coupling.md` (the SslEq assist-band consumer), and the future-work trackers `custom-lstm-controller-cell.md` (#16), `adaptive-eq-hf-band-tau.md` (#15), `ort_render_nondeterminism.md` (#19).

Test inventory for this module: `tests/test_adaptive_eq.cpp` (cascade: stability/adaptivity/direction/determinism/bypass), `tests/test_spectral_mask_eq.cpp` (flat reconstruction, varied-block boundedness, white-box OLA ring-overflow regression, stall, mask direction), `tests/test_iir_filterbank_eq.cpp` (unity/match/stability), `tests/test_autoeq_param_guard.cpp` (`kEqParamsStorage` contract), `tests/test_control_contract.cpp` (control read-set vs axon_meta), and `scripts/probe_auto_eq_adaptivity.py` (the CI-able mode-collapse tripwire).
