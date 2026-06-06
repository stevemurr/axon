# The Spectrum Analyzer

> A Goertzel-based, real-time spectrum visualizer that captures the audio at **every position in the Axon mastering chain** and feeds 128 log-spaced display points per stage (plus the AutoEQ curve) to the plugin's WebView UI — all on the main thread, with zero added audio latency.

All source references below point at `native/clap/src/axon_plugin.cpp`.

---

## 1. What is this?

A **spectrum analyzer** answers one question: *"How much energy is in my sound at each pitch?"*

Sound, as it travels through wires and your speakers, is a single wiggling value over time (a *waveform* — amplitude vs. **time**). That representation is great for "how loud, right now," but useless for "is my mix too bassy?" or "is there a harsh ringing at 3 kHz?" To answer those, you need the **frequency** view: amplitude vs. **frequency** (pitch).

Intuition / analogy:

- A **piano** is a spectrum analyzer you can play. Low notes on the left, high notes on the right. A spectrum analyzer takes a slice of your audio and tells you "how hard is each piano key being pressed right now."
- A **prism** splits white light into a rainbow — separating one combined beam into its constituent colors. The spectrum analyzer splits your one audio signal into its constituent frequencies.

The X axis is **frequency** in Hertz (Hz), drawn from **20 Hz** (the lowest rumble humans hear) up to **20,000 Hz / 20 kHz** (the highest sparkle). The Y axis is **amplitude** in **decibels (dB)** — how strong that frequency is.

```
 amplitude (dB)
   ^
 0 |        .-'''-.
   |      .'       '.        a "spectrum": one curve, energy at every pitch
-20|    .'           '-.__
   |  .'                  '''----..__
-40| '                              ''----
   +--------------------------------------> frequency (Hz, log scale)
     20    100   1k    3k    10k   20k
```

### Why a *mastering* UI needs *per-stage* spectra

Axon is a chain of DSP stages: an LSTM-driven **AutoEQ**, a **Saturator**, an **SSL-style bus compressor**, a **Mel limiter**, and a **Bass-Mono** stage (`enum class StageID`, lines 98–104). Each one *changes the spectrum*. The whole point of mastering is shaping that curve.

If you only saw the final output spectrum, you couldn't tell *which* stage did *what*. By capturing a spectrum at the **output of every stage**, the UI can overlay them so you can literally see the EQ scoop a notch, the saturator add harmonics up top, the limiter flatten the dynamics, etc. That is the per-stage tap design described in §9.

---

## 2. Why it matters

Mastering is a feedback loop: tweak a knob, listen, look, repeat. The "look" half is what this module provides.

- **Visual confirmation per stage.** You can see, in real time, the effect of each module independently, in the *current chain order* (the order is user-reorderable; see §9).
- **The AutoEQ curve overlay.** Because AutoEQ is a neural net predicting an EQ shape, there's no knob to "read." The analyzer separately latches the model's predicted gain curve (5-point and 50-point) so the UI can draw the filter response on top of the spectrum (lines 245–264).
- **Trustworthy, smoothed motion.** Raw spectra flicker violently frame-to-frame. The analyzer applies temporal smoothing (EMA, §4) so the display is readable rather than seizure-inducing, while still reacting in ~110 ms.

---

## 3. The DSP behind it (build the intuition, then get rigorous)

### 3a. The core idea: from a block of samples to "how much 3 kHz is in here?"

Take 2048 consecutive audio samples. We want to ask: "how strongly does a pure tone of frequency *f* appear in this block?" The classic answer is the **Discrete Fourier Transform (DFT)** — but a full DFT (via the FFT) computes *all* `N/2 = 1024` frequency bins. We only ever *draw* **128** points on screen. Computing 1024 bins to throw away 896 of them is wasteful, especially since we want **log-spaced** points (dense in the bass, sparse in the treble) that don't line up with the FFT's evenly-spaced bins anyway.

That is exactly the situation the **Goertzel algorithm** is built for.

### 3b. Why Goertzel instead of a full FFT

The Goertzel algorithm computes the DFT magnitude at **one** chosen frequency bin, using a tiny second-order IIR filter — no big butterfly network, no twiddle tables, no power-of-two padding gymnastics for a sparse target set. Cost per bin is `O(N)`; for `K` desired bins it is `O(K·N)`.

- Full FFT: `O(N log N)` to get **all** `N/2` bins.
- Goertzel for our case: `O(kDisp · N)` = `O(128 · 2048)`.

When `K` (number of wanted bins) is much smaller than `N/2`, and especially when those bins are *arbitrary / log-spaced* rather than the FFT's fixed grid, Goertzel is the natural, simple, allocation-free choice. This module wants exactly 128 **log-spaced** bins, so Goertzel fits perfectly.

### 3c. The Goertzel recurrence (exact math, from the code)

For a real input block `x[0..N-1]` and a (possibly fractional) target bin `bin_f`, the implementation (`goertzel`, lines 271–283) does:

```cpp
const int    k     = static_cast<int>(std::round(bin_f));
const double w     = 2.0 * M_PI * static_cast<double>(std::clamp(k, 0, N/2));
const double coeff = 2.0 * std::cos(w / N);
double s1 = 0.0, s2 = 0.0;
for (int n = 0; n < N; ++n) {
    const double s0 = x[n] + coeff * s1 - s2;
    s2 = s1; s1 = s0;
}
const float power = static_cast<float>(s1*s1 + s2*s2 - coeff*s1*s2);
return std::sqrt(std::max(power, 0.f)) * (2.f / N);
```

In math, with the integer bin `k` and `ω = 2π·k / N`, `coeff = 2·cos(ω)`:

```
s[n] = x[n] + coeff·s[n-1] − s[n-2]          (run for n = 0 … N−1)

power = s1² + s2² − coeff·s1·s2              (s1 = s[N-1], s2 = s[N-2])

magnitude = sqrt(power) · (2 / N)
```

Notes a vet will want:

- **`coeff = 2·cos(2πk/N)`** is the single per-bin constant; the inner loop is two multiplies and two adds per sample. That is the whole "filter."
- **`power = s1² + s2² − coeff·s1·s2`** is the standard squared-magnitude readout of the Goertzel state — equivalent to `|X[k]|²` for that bin.
- **`(2/N)` scaling** converts the DFT magnitude to (approximately) the **single-sided amplitude** of a real sinusoid of that frequency.
- **Bin snapping.** Although `bin_f` is computed as a *fractional* bin (`disp_hz[b] * kFFT / sr`, line 218), the implementation **rounds** it to the nearest integer bin `k` and **clamps** to `[0, N/2]`. So Goertzel is run at the nearest on-grid frequency, not a true off-grid Goertzel. With `N = 2048` the grid spacing at, say, 44.1 kHz is `sr/N ≈ 21.5 Hz`, fine in the treble but coarse in the deep bass — see §8.

### 3d. Hann windowing (why we don't feed raw samples)

A 2048-sample block is a *finite* slice of a continuous signal. Chopping it abruptly creates fake high-frequency content at the edges ("spectral leakage" — the rectangular-window sinc skirts). To suppress that, the block is multiplied by a **Hann window** before Goertzel runs.

The window is precomputed once in `init()` (lines 154–155):

```cpp
for (int i = 0; i < kFFT; ++i)
    hann[i] = 0.5f * (1.f - std::cos(2.f * M_PI * i / kFFT));
```

i.e. `w[i] = 0.5·(1 − cos(2π·i / N))` — a raised-cosine bell that tapers smoothly to zero at both ends, trading a slightly wider main lobe for dramatically lower side-lobes (cleaner-looking spectrum). It is applied per stage right before the Goertzel loop (lines 215–216):

```cpp
for (int i = 0; i < kFFT; ++i)
    windowed[i] = mt_frames[pos][i] * hann[i];
```

(Note: the `(2/N)` amplitude scaling does not compensate for the Hann window's coherent-gain loss of 0.5, so absolute dB values are offset by a constant; this is a *relative* analyzer/visualizer, not a calibrated measurement instrument.)

### 3e. Log-spaced display bins (20 Hz – 20 kHz)

Human pitch perception is logarithmic — an octave (a 2× frequency ratio) sounds like an equal "step" whether it's 100→200 Hz or 5k→10k Hz. So the display frequencies are spaced **geometrically** between `kFlo = 20 Hz` and `kFhi = 20000 Hz`, precomputed in `init()` (lines 156–157):

```cpp
for (int i = 0; i < kDisp; ++i)
    disp_hz[i] = kFlo * std::pow(kFhi / kFlo, float(i) / (kDisp - 1));
```

i.e. `disp_hz[i] = 20 · (20000/20)^(i/(127))` for `i = 0 … 127`. Bin 0 is exactly 20 Hz, bin 127 is exactly 20 kHz, and every step multiplies frequency by the constant ratio `1000^(1/127) ≈ 1.0553`.

### 3f. EMA temporal smoothing (kAlpha ≈ 0.65, ~110 ms)

Each display bin's magnitude is fed through a one-pole **Exponential Moving Average** (lines 219–220):

```cpp
const float mag = goertzel(windowed.data(), kFFT, bin_f);
ema[pos][b] = kAlpha * ema[pos][b] + (1.f - kAlpha) * mag;
```

With `kAlpha = 0.65f`, each new frame contributes 35% and the running history keeps 65%. At the analyzer's update cadence (~21 fps, ~48 ms per frame; see §6) the smoother's effective time constant is roughly **~110 ms** (as the source comment on line 114 states). This is the line that turns jittery raw magnitudes into a readable, gently-moving curve.

### 3g. The constants, all in one place

From lines 80–81 and 111–116:

| Constant   | Value      | Meaning |
|------------|-----------:|---------|
| `kBlockSize` | `128`    | Audio process block (samples) — also the spectrum push granularity. |
| `kNumStages` | `5`      | Number of chain positions tapped (one accumulator each). |
| `kFFT`     | `2048`     | Accumulation window length per frame (samples). |
| `kDisp`    | `128`      | Log-spaced display bins per stage. |
| `kNumBins` | `50`       | Resolution of the AutoEQ "eq_bins" curve (log-spaced 20–20 k). |
| `kAlpha`   | `0.65f`    | EMA coefficient (~110 ms at ~21 fps). |
| `kFlo`     | `20.f`     | Lowest display/curve frequency (Hz). |
| `kFhi`     | `20000.f`  | Highest display/curve frequency (Hz). |

---

## 4. How it works in the code (producer/consumer walkthrough)

The module is split across **two threads** with a mutex-guarded hand-off buffer. This is the heart of its design.

```
   AUDIO THREAD (real-time, must never block)          MAIN THREAD (UI, may block briefly)
   ─────────────────────────────────────────          ───────────────────────────────────
   per 128-sample block, per stage:                    plugin_on_main_thread():
     spectrum.push(pos, L, R, n_ch, 128)  ──fills──►     process_if_ready(sr):
     accum[pos].buf  (ring of 2048)                        lock xfer_mtx (lock_guard)
                                                            copy xfer_* → mt_*
   when accum[0].fill == 2048:                              unlock
     advance_and_transfer():                                for each stage:
       try_lock xfer_mtx (NON-blocking)                       window + 128× Goertzel
       copy accum[].buf → xfer_frames[]   ──hands off──►       EMA into ema[pos][b]
       snapshot eq gains/bins                              build_js(order) → JSON
       xfer_ready = true; unlock                          axon_gui_eval_js(...)
       reset all fills = 0
       request_callback(host)  ───wakes──►
```

### 4a. Audio-thread side: accumulation

The per-stage block processor `flush_chain_block_` runs each stage in `processor_order`, and **immediately after each stage** captures that stage's output (line 1491–1492):

```cpp
// Capture stage output for the spectrum analyzer.
plug.spectrum.push(pos, work_l, work_r, n_ch, kBlockSize);
```

`push` (lines 162–173) appends up to `kFFT - fill` samples into that position's ring `accum[pos].buf`. If stereo (`n_ch >= 2`) it stores the **mono average** `0.5*(L+R)`; otherwise it copies L. Once a buffer is full it simply ignores further pushes (`if (a.fill >= kFFT) return;`) until reset.

The AutoEQ case additionally latches the model's predicted EQ curve for the overlay (lines 1249–1265): a **5-point** curve at historical PEQ centres via `set_eq_gains` (lines 176–178), and a **50-point** log-spaced curve via `set_eq_bins` (lines 180–183, which also sets `xfer_has_bins = true`). Non-spectral-mask EQ classes would call `clear_eq_bins()` (line 184) so the UI draws `null` instead.

After all stages run, the block processor checks whether a full 2048-sample frame is ready and, if so, wakes the main thread (lines 1495–1497):

```cpp
if (plug.spectrum.advance_and_transfer())
    plug.host->request_callback(plug.host);
```

`advance_and_transfer` (lines 188–200) is the crucial real-time-safe hand-off:

- Returns immediately if `accum[0].fill < kFFT` (not a full frame yet).
- Uses **`try_lock`** (non-blocking) on `xfer_mtx`. If the main thread happens to hold the lock, the audio thread **does not wait** — it skips the copy this round.
- On success it copies all `kNumStages` accumulator buffers into `xfer_frames`, snapshots the EQ gains/bins, sets `xfer_ready = true`, unlocks.
- **Regardless** of whether the lock was acquired, it resets every `accum[].fill = 0` and returns `true`, so the host gets a callback. (If the lock was missed, that frame's spectra are simply dropped — acceptable for a visualizer.)

### 4b. Main-thread side: Goertzel + EMA + JSON

`plugin_on_main_thread` (lines 1846–1858) is invoked via the host callback. It calls `process_if_ready` then `build_js`:

```cpp
if (plug->spectrum.process_if_ready(plug->sample_rate)) {
    const std::string js = plug->spectrum.build_js(plug->processor_order);
    axon_gui_eval_js(plug->gui_state, js.c_str());
}
```

`process_if_ready` (lines 203–224):

1. Under a `lock_guard` on `xfer_mtx`, bails if `!xfer_ready`; else copies `xfer_frames → mt_frames` and the EQ snapshots into the `mt_*` members, clears `xfer_ready`, releases the lock. *All heavy DSP happens after the lock is released* — the critical section is just memory copies.
2. For each of the 5 stage positions: window the frame (Hann), then for each of the 128 display bins compute the fractional bin `bin_f = disp_hz[b] * kFFT / sr`, run `goertzel`, and fold the result into `ema[pos][b]`.

`build_js` (lines 227–267) serializes a single JS call `axonSpectrum({...})` containing:

- `"order"`: the current `processor_order` (so the UI knows which stage each curve belongs to).
- `"db"`: a `[kNumStages][kDisp]` array of magnitudes in dB, computed as `20·log10(max(ema, 1e-9))` (lines 239–240) — the `1e-9` floor avoids `log10(0)`.
- `"eq"`: the 5 LSTM EQ band gains in dB (the curve overlay).
- `"eq_bins"`: the 50-point curve **or `null`** depending on `mt_has_bins`.

The string is handed to the WebView via `axon_gui_eval_js`, where the front-end's `axonSpectrum(...)` function draws it.

### 4c. The lock-free-*ish* split, summarized

It is not strictly lock-free, but it is **wait-free on the audio thread**: the audio thread only ever uses `try_lock`, never blocks, and on contention just drops a frame. The main thread uses a blocking `lock_guard`, but only to copy buffers — never while running Goertzel. This is the standard single-producer/single-consumer "snapshot under a try-lock" pattern for shipping audio-thread data to a UI.

---

## 5. Latency & performance

- **Zero audio-path latency.** The analyzer is a *tap*, not an *insert*. `push` reads `work_l/work_r` but never writes them back; nothing in the audio output path depends on the analyzer. Removing it would not change a single output sample (or its latency).
- **Update rate ≈ 21 fps.** A new frame is ready every `kFFT = 2048` samples. At 44.1 kHz that is `44100/2048 ≈ 21.5` updates/sec (~47.6 ms/frame); at 48 kHz, `48000/2048 ≈ 23.4`. The "~21 fps" in the comments assumes ~44.1 kHz. (Note: pushes accumulate at the granularity of `kBlockSize = 128`, so a frame completes after `2048/128 = 16` audio blocks.)
- **Audio-thread cost is tiny and bounded.** Per block the audio thread does, per stage, an averaging copy of ≤128 samples; the only "heavy" audio-thread action is the `advance_and_transfer` buffer copy, which happens **once per 2048 samples** and only when the `try_lock` succeeds — no allocation, no syscalls, no blocking.
- **Main-thread cost.** Per frame: `kNumStages · kDisp = 5 · 128 = 640` Goertzel runs of length `N = 2048` each ≈ **1.3 M** inner iterations, plus the JSON build. This runs ~21×/sec on the main (GUI) thread, well away from the audio deadline.
- **Real-time safety.** The audio thread never blocks (try-lock), never allocates (all buffers are fixed `std::array`), and degrades gracefully (drops a visual frame) under lock contention.

---

## 6. Parameters / config constants (what they trade off)

| Constant | Raising it… | Lowering it… |
|----------|-------------|--------------|
| `kFFT` (2048) | finer frequency resolution (smaller `sr/N` grid, better bass detail) and slower update rate; more main-thread CPU per Goertzel run. | faster updates / more responsive but coarser bins and worse low-frequency resolution. |
| `kDisp` (128) | smoother-looking curve, more Goertzel runs (linear CPU cost), more JSON. | cheaper, blockier display. |
| `kNumBins` (50) | finer AutoEQ overlay curve. | coarser overlay; cheaper JSON. (Does not affect the spectrum itself.) |
| `kAlpha` (0.65) | *more* smoothing → slower, calmer display (longer effective time constant). | *less* smoothing → snappier but jumpier display. |
| `kFlo` / `kFhi` (20 / 20000) | shifts the visible frequency window; both endpoints are pinned to display bins 0 and 127. | — |
| `kBlockSize` (128) | This is the audio block size, fixed to the ONNX models' cond block size (line 78–80). It is the push/accumulation granularity, **not** freely tunable here. | — |

Key relationships:

- **Frequency resolution** = `sr / kFFT` (≈ 21.5 Hz at 44.1 kHz). Set by `kFFT`.
- **Update period** = `kFFT / sr` seconds. Also set by `kFFT` — so resolution and responsiveness are directly traded off by the same knob.
- **Smoothing time constant** ≈ `−(kFFT/sr) / ln(kAlpha)` ≈ `47.6 ms / 0.43 ≈ 110 ms`. Set by `kAlpha` (and indirectly `kFFT`).

---

## 7. Gotchas / things to watch

- **Bin snapping coarsens the bass.** `goertzel` rounds the fractional `bin_f` to the nearest integer bin (line 272). With `sr/N ≈ 21.5 Hz` resolution, several adjacent *log-spaced display points* below ~100 Hz can round to the **same** FFT bin — so the lowest octaves are quantized/duplicated. Increasing `kFFT` is the only way to refine this (at the cost of update rate). A true fractional Goertzel would help but isn't implemented.
- **Relative, not calibrated, dB.** The `(2/N)` scaling does **not** account for the Hann window's 0.5 coherent gain, and there's no equal-energy or pink-noise tilt. Treat the dB axis as *relative between stages/frames*, not as an absolute SPL/dBFS reading.
- **Dropped frames under contention are silent and expected.** If the audio thread's `try_lock` fails (main thread mid-copy), that whole frame's spectra are discarded and fills are reset anyway. It's a visualizer; this is fine, but don't rely on the analyzer for sample-accurate measurement.
- **`accum[0].fill` is the gate.** `advance_and_transfer` only checks position 0's fill (line 189). All positions are pushed the same `kBlockSize` count every block, so they stay in lock-step — but if that invariant were ever broken (e.g. a stage early-`break`s *before* the push), the assumption would too. As written, the push at line 1492 is **outside** the switch, so every stage always pushes, preserving the invariant.
- **Mutex on the audio thread.** Even a `try_lock` touches a mutex on the audio thread. It is non-blocking and only attempted once per 2048 samples, so it's safe in practice, but it's the one place to keep an eye on if real-time guarantees ever tighten.
- **Stereo is folded to mono.** Per-channel imbalance (e.g. a hard-panned hiss) is averaged away; the analyzer shows the mono sum, not L/R separately.
- **The `eq`/`eq_bins` fields are AutoEQ-specific.** They reflect the *last* AutoEQ block's prediction (channel 0 only, lines 1246–1265). If AutoEQ isn't in the chain or isn't a spectral-mask class, `eq_bins` is `null` and `eq` holds stale/zero values.

---

## 8. Where it sits in the Axon chain (taps per stage)

The analyzer keeps **one accumulator per chain position** (`std::array<Accum, kNumStages> accum`, line 123). The chain is **user-reorderable**; the active order lives in `plug.processor_order` (default `{1, 2, 4, 6, 5}`, line 667 — i.e. AutoEQ → Saturator → SslComp → BassMono → MelLimiter). Position index `pos` (0–4), **not** stage ID, is what the analyzer keys on, and `build_js` ships `processor_order` so the UI can map each curve to its stage (lines 227–231).

```
            ┌──────────┐   ┌───────────┐   ┌─────────┐   ┌──────────┐   ┌───────────┐
 input ───► │ stage[0] │─► │ stage[1]  │─► │stage[2] │─► │ stage[3] │─► │ stage[4]  │─► (Trim + TruePeakCeiling) ─► output
            └────┬─────┘   └─────┬─────┘   └────┬────┘   └────┬─────┘   └─────┬─────┘
                 │push(0)        │push(1)       │push(2)      │push(3)        │push(4)
                 ▼               ▼              ▼             ▼               ▼
            accum[0]        accum[1]       accum[2]      accum[3]        accum[4]
            (output of      (output of     ...           ...             (output of last
             stage[0])       stage[1])                                    user stage)
```

Each tap is the **output** of the stage at that position (the push happens after the stage's switch case, line 1492). Stage IDs themselves are `AutoEQ=1, Saturator=2, SslComp=4, MelLimiter=5, BassMono=6` (lines 98–104; IDs 0 and 3 are intentionally retired). The final **Trim + TruePeakCeiling** stage runs *after* the loop and is **not** tapped by the analyzer (it's the non-reorderable true master limiter, lines 1499–1508) — so the analyzer shows the chain *up to* but not including the brickwall ceiling.

---

## 9. Related modules

- **AutoEQ (`StageID::AutoEQ`, lines 1200–1270).** Produces the EQ-curve overlay the analyzer carries in `eq` / `eq_bins`. It samples its predicted gains at historical PEQ centres (5 points, `kDisplayHz`) and at 50 log-spaced points (`kBinHz`), then hands them to `set_eq_gains` / `set_eq_bins`. The analyzer is the transport that gets those curves to the WebView; AutoEQ is where they originate. See `docs`/memory note *project_meter.md* and the AutoEQ DSP for the curve generation itself.
- **Loudness Meter (`meter_in` / `meter_out`).** A sibling visualizer pushing LUFS/RMS/Peak readings to the UI on the **same ~21 fps main-thread cadence** (`axonMeters(...)`, lines 1860+). It is the *level/loudness* counterpart to this *spectrum* analyzer; both are display-only taps with no audio-path effect. `meter_in` taps the raw pre-chain input (line 1193).
- **The chain stages themselves** (Saturator, SslComp, MelLimiter, BassMono) — each contributes one tapped position whose output the analyzer renders.

---

### Appendix: quick method map

| Method | Thread | Lines | Role |
|--------|--------|------:|------|
| `init()` | setup | 153–159 | precompute Hann window + log display freqs, zero EMA |
| `push(pos,L,R,n_ch,n)` | audio | 162–173 | append (mono-averaged) samples into `accum[pos]` |
| `set_eq_gains(...)` | audio | 176–178 | latch 5-point AutoEQ curve |
| `set_eq_bins(...)` / `clear_eq_bins()` | audio | 180–184 | latch / clear 50-point AutoEQ curve |
| `advance_and_transfer()` | audio | 188–200 | try-lock snapshot of all accumulators → xfer; reset fills |
| `process_if_ready(sr)` | main | 203–224 | copy xfer→mt under lock, then Hann + Goertzel + EMA |
| `build_js(order)` | main | 227–267 | serialize `axonSpectrum({...})` JSON for the WebView |
| `goertzel(x,N,bin_f)` | main | 271–283 | single-bin DFT magnitude |
