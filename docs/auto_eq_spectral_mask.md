# Auto EQ — the Spectral-Mask EQ

> A neural-controlled equalizer that re-shapes the tonal balance of audio by
> multiplying its short-time spectrum with a per-frequency gain mask, where the
> mask is predicted block-by-block by a small LSTM "controller" model.

Primary source: [`native/clap/src/spectral_mask_eq.hpp`](../native/clap/src/spectral_mask_eq.hpp)
Wiring/context: [`native/clap/src/axon_plugin.cpp`](../native/clap/src/axon_plugin.cpp) (the `StageID::AutoEQ` case, around line 1200)

---

## 1. What is this?

An **equalizer (EQ)** is a tool that makes some frequencies louder and others
quieter. Think of the bass/mid/treble knobs on a car stereo: turn up "bass" and
the low rumble gets louder; turn up "treble" and the cymbals get brighter. A
mastering EQ does the same thing but with many more, finer controls, and far
more gently.

This module — **Auto EQ**, internally the `SpectralMaskEq` class — is an EQ with
two unusual properties:

1. **It is automatic.** You don't dial in the curve by hand. A small neural
   network (an LSTM "controller") *listens* to the audio and decides, several
   hundred times per second, how much to boost or cut in each of 64 frequency
   bands. The C++ code here is the *engine* that takes those predictions and
   actually applies them to the sound.

2. **It works in the frequency domain via a "spectral mask".** Instead of
   building classic analog-style filters, it chops the audio into short
   overlapping chunks, converts each chunk into a *spectrum* (a list of how much
   energy lives at each frequency), multiplies that spectrum by a gain curve (the
   "mask"), and converts back to audio. That gain curve is the mask.

### Three concepts you need

**Frequency.** Any sound is a mix of pure tones at different pitches
(frequencies, measured in Hertz / Hz). 60 Hz is a low bass note; 10,000 Hz
(10 kHz) is "air"/sparkle. Humans hear roughly 20 Hz–20,000 Hz.

**The FFT / STFT.** The *Fourier transform* is a mathematical machine that takes
a chunk of audio (a list of samples over time) and tells you "how much of each
frequency is in here". The **FFT** (Fast Fourier Transform) is the efficient
algorithm for it. Because audio changes over time, we don't transform the whole
song at once — we slide a short window along the audio and transform each window
separately. That sliding-window version is the **STFT** (Short-Time Fourier
Transform). Each windowed transform is called a *frame*.

> Analogy: an FFT of one frame is like a single vertical slice of a
> spectrogram — the colorful "frequency vs. time" picture you see in audio
> editors. The STFT is the whole picture, slice by slice.

**Mel bands.** Human hearing is *not* linear in frequency. The gap between
100 Hz and 200 Hz sounds huge (an octave), while 10,000 Hz to 10,100 Hz is
inaudible as a pitch change. The **mel scale** warps frequency so that equal
steps sound equally spaced to us. This module divides the spectrum into
**64 mel-spaced bands** — narrow in the bass, wide in the treble — which is why
the neural controller only has to output 64 numbers instead of one per FFT bin
(there are 2049 bins). The 64 band gains are then spread back out onto all the
bins via a triangular filterbank.

---

## 2. Why it matters in mastering

Mastering is the final polish on a finished mix: getting the overall tone
balanced, consistent, and translating well on every playback system. Tonal
balance — not too boomy, not too harsh, not too dull — is the single biggest
lever, and it is exactly what an EQ controls.

A human mastering engineer makes small, broad EQ moves (a dB here, half a dB
there) based on experience and reference tracks. Auto EQ replaces the
*judgement* step with a model trained on professionally mastered audio: it
predicts the broad tonal correction this material "wants", per signal class
(bass, drums, vocals, etc.), and applies it transparently. The operator keeps
high-level control — how much of it to apply (Range), how fast it reacts
(Speed), how much wet signal to blend (the Auto EQ amount), and whether boosts
are attenuated (Boost) — but the per-band decision-making is automated.

Because it is a **mastering** EQ, two engineering choices matter a lot and show
up in this code:

- **Smoothness.** The mask is smoothed across frequency (1/6-octave) and across
  time (a one-pole smoother) so the result is a gentle broad-stroke curve, not a
  spiky surgical filter that would sound artificial.
- **Minimum phase.** It avoids the "pre-ring" smearing of transients (kick
  drums, cymbals) that a naive zero-phase frequency-domain EQ produces — see
  §4.4.

---

## 3. Where the 64 band gains come from

The C++ module does **not** decide *what* to boost; it only knows *how* to apply
a decision. The decision is the neural controller's job.

```
                     ┌─────────────────────────────┐
 audio block (128) ─▶│ peak-normalise to match the │
                     │ training input distribution │
                     └──────────────┬──────────────┘
                                    ▼
                     ┌─────────────────────────────┐
                     │ LSTM controller (ONNX/ORT)  │  per-class model:
                     │  → 64 sigmoid values [0,1]  │  bass/drums/vocals/
                     └──────────────┬──────────────┘  other/full_mix
                                    ▼  set_params(params, 64)
                     ┌─────────────────────────────┐
 audio block (128) ─▶│       SpectralMaskEq        │─▶ EQ'd block
                     │  (this module — the engine) │
                     └─────────────────────────────┘
```

In `axon_plugin.cpp`, the `StageID::AutoEQ` case does this every 128-sample
block (`axon_plugin.cpp:1200`–`1268`):

1. Pick the model for the active **EQ Class** (`CLS`); if the class changed,
   reset the LSTM hidden state so it doesn't carry over activations from a
   different signal type (`axon_plugin.cpp:1204`–`1211`).
2. Peak-hold normalise the block to ~0.5 peak so the controller sees the level
   distribution it was trained on (`axon_plugin.cpp:1222`–`1232`).
3. Run the controller → 64 sigmoid values in `[0, 1]`
   (`run_controller`, `axon_plugin.cpp:1233`).
4. Push runtime knobs (Range, Boost, Speed) and the 64 values into the engine
   via `set_range_norm` / `set_boost_scale` / `set_speed_tau_ms` / `set_params`
   (`axon_plugin.cpp:1241`–`1244`).
5. `process()` the block and wet/dry blend by the Auto EQ amount
   (`axon_plugin.cpp:1245`, `1267`).

The controller is an LSTM with a tiny receptive field (`receptive_field: 1`,
`architecture: lstm`, hidden size 64 — from each class's `plugin_meta.json`),
which is why it must be fed every block and why its `root_h`/`root_c` state is
carried across blocks.

---

## 4. The DSP behind it

This is the heart of the module. Everything below is exactly what the code does,
with the real constants from the shipped models
(`weights/axon_bundle/auto_eq_*/plugin_meta.json`):

| Constant | Value | Meaning |
|---|---|---|
| `sample_rate` | 44100 | samples per second |
| `n_fft` | **4096** | FFT/window size (samples) |
| `hop` | **2048** | samples between successive frames (50% overlap) |
| `n_bands` | **64** | mel control bands |
| `n_freq` | `n_fft/2 + 1` = **2049** | usable FFT bins (real input) |
| `block_size` | 128 | controller call cadence |
| `min_gain_db` / `max_gain_db` | **−18 / +18 dB** | trained per-band gain span |
| `f_min` / `f_max` | **30 / 22050 Hz** | mel band frequency range |

### 4.1 STFT with Hann windows and overlap-add (OLA)

The classic streaming-EQ recipe:

1. **Window.** Multiply each `n_fft`-sample frame by a **Hann window** to taper
   its edges to zero, which prevents spectral "leakage" artifacts at the frame
   boundaries. The Hann window is built in `reset()`
   ([`spectral_mask_eq.hpp:75`](../native/clap/src/spectral_mask_eq.hpp#L75)):

   ```cpp
   window_[n] = 0.5f * (1.0f - std::cos(2.0f * M_PI * n / n_fft));
   ```

2. **Forward FFT** of the windowed frame → spectrum (per-bin complex values).
3. **Apply the mask** — multiply the spectrum by the per-bin gain filter (§4.4).
4. **Inverse FFT** → back to a time-domain frame.
5. **Synthesis window + overlap-add.** Multiply the output frame by the Hann
   window *again* and add it, overlapping, into a running output buffer. Using
   the window on both analysis and synthesis (a "Hann²" / `window²` envelope)
   makes the overlapping frames sum smoothly.

Because both windows are applied, the overlapping windows do **not** sum to a
constant. The code corrects for this exactly like `torch.istft`: it accumulates
the sum of `window²` in a parallel `norm_ring_` buffer and divides each output
sample by its accumulated `window²` value
([`spectral_mask_eq.hpp:237`](../native/clap/src/spectral_mask_eq.hpp#L237)–`240`).
This is **per-sample normalisation**, which is robust even if the
constant-overlap-add (COLA) sum drifts between 0.5 and 1.0 over the hop cycle
(see the `ola_scale_` comment at line 133–139).

With `hop = n_fft / 2 = 2048`, there is exactly **50% overlap** (two frames
contribute to every output sample).

```
frame k     [=====Hann window 4096=====]
frame k+1            [=====Hann window 4096=====]
frame k+2                     [=====Hann window 4096=====]
            |--hop--|--hop--|     hop = 2048, 50% overlap
output = (windowed IFFT frames, overlap-added) / Σ window²
```

### 4.2 Mel band edges — the HTK formula

The 64 control bands are spaced on the mel scale using the **HTK** formula
(`build_mel_`, [`spectral_mask_eq.hpp:468`](../native/clap/src/spectral_mask_eq.hpp#L468)):

Hz → mel:   `mel = 2595 · log10(1 + f / 700)`
mel → Hz:   `f  = 700 · (10^(mel/2595) − 1)`

The code computes `mel_min`/`mel_max` from `f_min`/`f_max`, lays down
`n_bands + 2 = 66` equally-spaced points in mel, converts each back to Hz, then
to an FFT-bin index. Adjacent triple-points define **triangular** band filters
(rising from the left edge to the center, falling to the right edge):

```cpp
const float up = (k - left)  / l_span;   // rising edge
const float dn = (right - k) / r_span;   // falling edge
float w = std::min(up, dn);              // triangle
```

These weights form a `band_to_bin_` matrix `[n_bands × n_freq]`
(`spectral_mask_eq.hpp:482`–`497`). A companion `bin_norm_[k]` holds the sum of
all band weights at bin `k` (line 498–503), used to normalise when projecting
band gains onto bins.

### 4.3 Band gains → per-bin dB → smoothing (`set_params`)

`set_params` ([`spectral_mask_eq.hpp:161`](../native/clap/src/spectral_mask_eq.hpp#L161))
turns the controller's 64 sigmoid values into the per-bin gain mask:

1. **Sigmoid → dB per band** (line 174–180). Each value `g ∈ [0,1]` maps onto
   the trained span:
   `db = (min_gain_db + g · (max_gain_db − min_gain_db)) · range_norm`
   = `(−18 + g·36) · Range`. So `g = 0.5` is 0 dB, `g = 1` is +18 dB, `g = 0`
   is −18 dB. **Boost** then attenuates only positive dB:
   `band_db = db > 0 ? db · boost_scale : db`.

2. **Band → bin (dB domain)** via a vectorised `vDSP_vsma` accumulate over the
   `band_to_bin_` matrix, then divide by `bin_norm_` (line 184–194). Result:
   a smooth per-bin dB curve interpolated across the 2049 bins.

3. **Frequency smoothing** with a **1/6-octave Gaussian kernel**, in the dB
   domain (line 200–209). The kernel is precomputed per output bin with a sigma
   that scales with frequency (constant fraction of an octave), so bass bins get
   a ~1-bin kernel and treble bins get a wide one — perceptually uniform
   smoothing (`build_freq_smoothing_kernel_`, line 418). Smoothing in dB ≡
   geometric mean in linear, which behaves well across big bin-to-bin dB jumps.
   Output is `bin_gain_target_[k] = 10^(acc/20)` (linear).

4. **Time smoothing** — a one-pole IIR toward the target, advanced once per
   `set_params` tick (line 213–217):
   `bin_gain_[k] = α·bin_gain_[k] + (1−α)·bin_gain_target_[k]`
   `set_params` runs every `block_size` = 128 samples (~2.9 ms at 44.1 k), but
   the FFT only consumes `bin_gain_` every `hop` = 2048 samples (~46 ms). Without
   this smoother, the ~21 Hz frame-rate stepping of the mask shows up as
   graininess on kick/bass. `α` comes from the Speed time constant (§7) via
   `recompute_alpha_` (line 410).

### 4.4 Minimum-phase mask application

A frequency-domain EQ has to decide what *phase* the filter has. The simplest
choice — apply the real magnitude mask directly to every bin — yields a
**zero-phase** (linear-phase-like) filter whose impulse response is symmetric.
That symmetry causes **pre-ring**: a cut at high frequencies smears energy
*backwards in time* into the silence *before* a transient, audibly softening
kicks and cymbals.

This module instead builds a **minimum-phase** filter from the same magnitude,
so the impulse response is **causal** (energy only after the event, no pre-ring),
with identical `|H|`. This is done per frame via the **real-cepstrum** method
(Oppenheim & Schafer), in `compute_min_phase_`
([`spectral_mask_eq.hpp:355`](../native/clap/src/spectral_mask_eq.hpp#L355)):

1. `log_mag[k] = log(max(mag[k], 1e-7))`  (−140 dB floor)
2. Real cepstrum `c[n] = IDFT{log_mag}`
3. Fold the anti-causal half onto the causal half:
   `w[0]=1, w[1..N/2−1]=2, w[N/2]=1, w[N/2+1..N−1]=0` → `c_min`
4. `log H_mp[k] = DFT{c_min}`
5. `H_mp[k] = exp(log H_mp[k])` (complex)

`H_mp` is then applied as a **complex multiply** against the input spectrum
(`run_frame_`, line 295–308): DC and Nyquist (which are real for a real-cepstrum
min-phase filter) are scaled directly, and bins `1..N/2−1` go through
`vDSP_zvmul`.

> Why min-phase changes the latency story: see §6.

---

## 5. How it works in the code

### Key structs / functions

| Symbol | Location | Role |
|---|---|---|
| `SpectralMaskEqParams` | `meta.hpp:40` | geometry config (n_fft, hop, bands, gain/freq ranges) |
| `SpectralMaskEq::reset()` | `spectral_mask_eq.hpp:51` | allocate buffers, build window/mel/kernel, init FFT setup |
| `set_params()` | `spectral_mask_eq.hpp:161` | controller 64 values → smoothed per-bin gain mask |
| `process()` | `spectral_mask_eq.hpp:221` | streaming ring-buffer push/pull, runs frames |
| `run_frame_()` | `spectral_mask_eq.hpp:268` | one STFT frame: window → FFT → mask → IFFT → OLA |
| `compute_min_phase_()` | `spectral_mask_eq.hpp:355` | magnitude mask → causal complex filter |
| `build_mel_()` | `spectral_mask_eq.hpp:468` | HTK mel triangular filterbank |
| `build_freq_smoothing_kernel_()` | `spectral_mask_eq.hpp:418` | 1/6-octave Gaussian kernel |
| `sample_gains_db()` | `spectral_mask_eq.hpp:253` | sample mask at given Hz for the GUI curve |
| `latency_samples()` | `spectral_mask_eq.hpp:263` | returns `n_fft` |

### The streaming ring-buffer contract

The host calls `process(in, out, n)` with an arbitrary `n` (commonly 128). The
module decouples that from its internal frame rate using ring buffers
([`spectral_mask_eq.hpp:221`](../native/clap/src/spectral_mask_eq.hpp#L221)):

- **Input accumulation.** Each incoming sample is written into the `n_fft`-sized
  `in_ring_`; a `samples_since_` counter triggers `run_frame_()` every `hop`
  samples (line 224–232).
- **Frame processing.** `run_frame_()` reads the ring oldest-first (two
  contiguous `vDSP_vmul` segments to avoid per-sample modulo), windows, FFTs,
  applies `H_mp`, IFFTs, applies the synthesis window and `ola_scale_`, and adds
  into `out_ring_` (audio) and `norm_ring_` (`window²`) — again as two contiguous
  segments (line 320–337). Then `out_write_` advances by `hop` and `out_avail_`
  grows by `hop`.
- **Output pull.** For each call, one finished output sample is returned,
  divided by its accumulated `window²` (line 237–244). If none is ready yet
  (start-up), it returns 0 — this is the latency priming.

`process()` is **in-place safe** (it reads `in[i]` before writing `out[i]`).

The output ring is sized `n_fft + hop` (line 83) so a freshly written frame
never overwrites samples still waiting to be read.

---

## 6. Latency & performance

### Exact latency: `n_fft` = **4096 samples**

`latency_samples()` returns `n_fft` (line 263), and `compute_latency_` in the
plugin adds `sp.n_fft` when the EQ is wet (`axon_plugin.cpp:851`). At 44.1 kHz
that is **~93 ms**.

> Note the subtlety (header comment, lines 16–20): a *zero-phase* STFT-EQ would
> only cost `n_fft − hop` = 2048 samples, because its symmetric impulse response
> can pre-ring into the leading zeros. But this filter is **minimum-phase /
> causal** — it has *no* pre-ring, so the first meaningful overlap-add output
> doesn't appear until ring position `hop`, consumed at output sample
> `2·hop − 1 ≈ n_fft`. Those extra `hop` samples are real and must be reported,
> so the honest latency is the full `n_fft`. (The stale `meta.hpp:39` /
> `meta.hpp` struct comment still says `n_fft − hop`; the runtime uses `n_fft` —
> trust the code.) Latency is only added when EQ wet > 0 and the active class is
> a spectral mask EQ.

### Performance — Accelerate / vDSP / vForce

The module is built on Apple **Accelerate** (macOS-only, matching the project's
build constraint):

- FFTs via `vDSP_create_fftsetup` + `vDSP_fft_zrip` (radix-2, real-input split
  complex), set up once in `reset()`.
- Windowing, the band→bin accumulate (`vDSP_vsma`), the complex mask multiply
  (`vDSP_zvmul`), and OLA adds (`vDSP_vadd`/`vDSP_vmul`/`vDSP_vsmul`) are all
  vectorised.
- The minimum-phase `exp/cos/sin` over the 2047 bins use **vForce**
  (`vvexpf`/`vvcosf`/`vvsinf`, line 400–406).

### Real-time safety

After `reset()`, the audio path allocates nothing: all buffers (`in_ring_`,
`out_ring_`, `norm_ring_`, FFT scratch, cepstrum scratch, `h_mp_*`) are
pre-sized. `set_params` and `process` are lock-free and allocation-free. There
are no CLAP/ONNX/`std::variant` dependencies in the header, so the class can be
unit-tested standalone (header comment, line 22–23).

One thing to be aware of: `compute_min_phase_` runs **two extra FFTs** (one
inverse, one forward) plus the vForce `exp/cos/sin` *per processed frame*
(`run_frame_`, line 295). That's the cost of causal (no pre-ring) application,
paid every `hop` = 2048 samples.

---

## 7. Parameters

The plugin exposes these controls (from `weights/axon_bundle/axon_meta.json`).
The first column is the CLAP control ID seen in `axon_plugin.cpp`.

| ID | Name | Range | Default | What it does |
|---|---|---|---|---|
| `EQ`  | Auto EQ   | 0–1            | 1.0   | Wet/dry blend of the EQ'd signal. Scaled by `amt_autoeq.wet_mix_max` then used in `blend_` (`axon_plugin.cpp:1152`, `1267`). 0 = bypass. |
| `CLS` | EQ Class  | 0–4 (enum)     | 4     | Which per-class controller model: 0=bass, 1=drums, 2=vocals, 3=other, 4=full_mix (`class_order` in axon_meta). Changing it resets the LSTM state. |
| `EQR` | EQ Range  | 0–1            | 1.0   | Scales the predicted dB curve toward 0 dB. `set_range_norm` → multiplies every band's dB (`spectral_mask_eq.hpp:145`, applied at line 178). 1 = full trained ±18 dB depth, 0 = flat. |
| `EQB` | EQ Boost  | 0–1            | 1.0   | Asymmetric attenuation of *boosts only*. `set_boost_scale` → positive band dB ×= boost (line 148, applied at 179). 1 = symmetric boost+cut; 0 = cut-only EQ. |
| `EQS` | EQ Speed  | 10–500 ms      | 100   | Time constant of the per-bin gain smoother. `set_speed_tau_ms` (line 154) → `recompute_alpha_` (line 410). ~10 ms = snappy/transient-tracking, ~500 ms = slow/mastering-style. |

**Per-band gains** (the 64 sigmoid values) are **not** user parameters — they
come from the neural controller every block (§3). The shipped per-band span is
`min_gain_db = −18`, `max_gain_db = +18` dB, set in the model's `plugin_meta.json`
and read into `SpectralMaskEqParams` (`meta.cpp:43`–`49`).

For the GUI, `sample_gains_db` is called at 5 historical PEQ band centres
(1010, 110, 1100, 7000, 10000 Hz) for the curve overlay and at 50 log-spaced
points (20 Hz–20 kHz) for the bin display (`axon_plugin.cpp:1246`–`1265`).

---

## 8. Gotchas / things to watch

- **Latency is `n_fft`, not `n_fft − hop`.** The header comment and runtime are
  correct (4096); the `SpectralMaskEqParams` struct comment in `meta.hpp:39`
  still says `n_fft − hop` and is stale. The full `n_fft` cost is *because* the
  filter is minimum-phase. (~93 ms at 44.1 k — significant; only added when
  EQ wet > 0.)
- **`n_fft` must be a power of two** (radix-2 vDSP); `reset()` throws otherwise
  (line 53–57). `hop` must be in `(0, n_fft]` (line 58–60).
- **Geometry must match across classes.** All auto-EQ classes must declare
  `spectral_mask_eq` with *identical* geometry (n_fft, hop, n_bands, gain/freq
  range) because the runtime `SpectralMaskEq` is shared and only the controller
  swaps per class (`axon_plugin.cpp:1982`–`1985`).
- **Class switch resets LSTM state.** Changing `CLS` zeroes the controller's
  hidden state and the peak envelope (`axon_plugin.cpp:1204`–`1211`) — expect a
  brief re-settle, not a glitch, but it's intentional.
- **Two smoothers, two clocks.** `set_params` runs every 128 samples; frames run
  every 2048. The time smoother (§4.3) exists specifically to bridge that gap;
  don't remove it expecting "snappier" EQ — you'll reintroduce ~21 Hz grain.
- **Range vs. Boost are different axes.** Range scales the *whole* curve (boosts
  and cuts) toward flat; Boost attenuates *only boosts*. Setting Boost = 0 gives
  a cut-only EQ even with Range = 1.
- **dB-domain smoothing means geometric averaging.** A +12 dB bin next to a
  −12 dB bin smooths toward 0 dB (geometric mean), not toward an arithmetic
  linear average — perceptually correct, but surprising if you expect linear
  blending.
- **Magnitude floor / DC & Nyquist.** The cepstrum floors magnitude at 1e-7
  (−140 dB); DC and Nyquist bins are handled as purely real special cases
  (line 299–300, 393–396). Don't "optimise" them into the vectorised loop.
- **Min-phase costs 2 extra FFTs per frame** (§6). If you ever need to trim CPU,
  this is the knob — but you'd trade it for pre-ring.

---

## 9. Where it sits in the Axon chain

Auto EQ is `StageID::AutoEQ` (= 1, `axon_plugin.cpp:99`) and is the **first**
stage in the default processor order (all stages are GUI-reorderable):

```
audio in
   │
   ▼
┌──────────┐   ┌───────────┐   ┌──────────┐   ┌──────────┐   ┌────────────┐
│ Auto EQ  │──▶│ Saturator │──▶│ SSL Comp │──▶│ BassMono │──▶│ MelLimiter │──▶ out
│ (this)   │   │           │   │ (bus)    │   │          │   │  (ceiling) │
└──────────┘   └───────────┘   └──────────┘   └──────────┘   └────────────┘
```

Default `processor_order = {1, 2, 4, 6, 5}` =
AutoEQ → Saturator → SslComp → BassMono → MelLimiter
(`axon_plugin.cpp:666`–`667`). Top-level signal path comment:
`audio → ort(autoeq controller) → SpectralMaskEq` (`axon_plugin.cpp:3`).

### Related modules

- **The LSTM controller (ORT/ONNX)** — supplies the 64 band gains; lives in the
  per-class `autoeq_ort_per_class` sessions (`axon_plugin.cpp:1221`).
- **MelLimiter** (`mel_limiter.cpp`) — also uses the HTK mel formula and an
  STFT, but for limiting rather than EQ; a useful cross-reference for the mel
  math (`mel_limiter.cpp:98`–`100`).
- **SpectrumAnalyzer** — consumes `sample_gains_db` output to draw the EQ curve
  overlay (`axon_plugin.cpp:1253`, `1265`).
- **Python `SpectralMaskEQ`** — the reference processor this header mirrors
  (same n_fft, hop, mel edges, Hann windows; header comment lines 1–7). This C++
  port adds the minimum-phase application and the two-stage (freq + time)
  smoothing.
