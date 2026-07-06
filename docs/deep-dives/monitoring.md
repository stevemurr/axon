# The Monitoring Stack: BS.1770 meters, a level-matched bypass, and a Goertzel spectrum that doubles as a data bus

Axon's whole job is to make things louder — an adaptive EQ, a bus compressor, a Mel limiter, a true-peak ceiling, all conspiring to raise LUFS. Which makes it exactly the kind of plugin that fools its own user: at monitoring volumes, *anything* half a dB louder sounds "better," so every knob you touch seems to help. The monitoring stack is the counterweight — four small units that exist to stop you from being deceived by the rest of the plugin:

- **`LoudnessMeter`** ×2 — honest BS.1770 in/out metering (short-term + momentary LUFS, RMS, decaying peak), with the OUT tap deliberately placed where Auto Gain can't touch it.
- **`AutoGain`** — a feed-forward monitoring trim that pins the *delivered* output to the *input's* loudness, so an A/B compares timbre and dynamics, not volume.
- **The internal Bypass FIFO** — a raw-input delay line that lags by exactly the plugin latency, so toggling Bypass is time-aligned to the sample.
- **`SpectrumAnalyzer`** — per-stage Goertzel spectra at ~21 fps, with a plot twist: its EQ-curve snapshot is the *input* to the SSL-EQ calibration solver, making the "visualizer" load-bearing DSP infrastructure that runs even with no GUI attached.

Measured cost of the entire stack (instrumented `AXON_STAGE_TIMING` build, buf=128, all-stages scenario — `native/clap/docs/perf_stage_ranking.md`): **MeterIn 0.253 %, MeterOut 0.252 %, SpectrumPush 0.027 %, AutoGain 0.011 % — about 0.54 % of process time combined.** Honesty is nearly free.

```
host in ──► [128-sample accumulator] ──► IN tap ──► reorderable stages ──► Trim ──► TruePeakCeiling
   │                                     meter_in   (spectrum tap after                  │
   │                                                 every stage)               ═ REAL MASTER ═
   └──► bypass_fifo (raw, delayed                                                        │
        by exactly the plugin latency)                                          OUT tap: meter_out
                            │                                                            │
                            │                                              × AutoGain (monitor-only trim)
                            ▼                                                            │
                     BYP on? ─── selects dry ◄────────────────── output ring ◄───────────┘
                            │
                            ▼
                        host out
```

Sources: `native/clap/src/meter.{hpp,cpp}`, `k_weighting.hpp`, `auto_gain.hpp`, `lufs_leveler.{hpp,cpp}` (test-only oracle), and the wiring in `axon_plugin.cpp`. All line anchors below are against `origin/main`.

---

## 1. Three meanings of "loud," and why the meter reports all of them

The `Readout` struct (`meter.hpp:26`) carries four numbers per stream, because "how loud" genuinely has more than one answer:

- **`peak_db`** — the tallest sample. Clipping headroom, not loudness: a snare hit can peak at −1 dBFS and still *sound* quiet.
- **`rms_db`** — mean power over ~300 ms, unweighted. The "thickness" of the signal.
- **`lufs_s` / `lufs_m`** — ITU-R BS.1770 loudness over 3 s (short-term) and 400 ms (momentary): RMS refined by **K-weighting** (a filter approximating how human hearing weights frequency) and **channel-power summing** (L²+R², so stereo reads consistently). This is the number streaming platforms normalize to (≈ −14 LUFS), so it's the number a mastering chain must aim with.

The whole unit is pure DSP — no CLAP, no ONNX dependencies — precisely so it can be unit-tested standalone and cross-checked against an independent implementation (§3).

### K-weighting: one shared header, two independent integrators

BS.1770's K-weighting is two cascaded biquads: a high-shelf "pre" filter (~+4 dB above ~1.5 kHz, modeling the head's acoustic effect) and the RLB 2nd-order high-pass (rolls off subsonic energy). The constants live in **`k_weighting.hpp`** — exact a0-normalized tables for 48 kHz (`kKCoeffs48000`, `k_weighting.hpp:28`) and 44.1 kHz (`kKCoeffs44100`, `:35`), matching the libebur128 reference implementation. Any other rate falls back to a proportional z-plane warp of the 48 kHz prototype (`:45-59`): with `s = 48000/sr`, the b1/a1 terms scale by `s` and the b2/a2 terms by `s²`. That is *not* the spec's continuous-time redesign, but LUFS integrates over seconds, which smooths the error — an accepted approximation, flagged in the header comment.

The header's own contract is worth quoting, because it's the design decision that keeps the test suite meaningful:

> ONLY the constants/selection live here. Each unit keeps its own biquad state/application and its own sub-block/ring integration machinery, so test_meter's LoudnessMeter-vs-LufsLeveler cross-check stays meaningful. (`k_weighting.hpp:10-12`)

I.e. the meter and its oracle share the *tables* (which are transcriptions of a standard, so sharing them loses nothing) but not the *machinery* (which is where bugs would live). This header is where the constants moved when they were deduplicated out of `meter.cpp` and `lufs_leveler.cpp`; older docs cite them at `meter.cpp:17-24`, which has drifted.

`meter.cpp:10-20` (`set_k_weighting_`) loads the coefficients into two `BiquadTDF2Stereo` filters — transposed direct form II with **separate left/right state registers**, so one meter instance K-weights a stereo pair with no filter-state cross-talk. A guard clamps `sr < 1.0` to 1.0 first (`meter.cpp:11`): without it, `48000.0/sr` at sr=0 is Inf, every coefficient goes Inf, and the first filtered sample produces Inf−Inf = NaN, poisoning the LUFS outputs. That exact failure is pinned by a regression test (§3).

### The 100 ms sub-block ring: O(1) sliding windows

`process()` (`meter.cpp:53-112`) runs once per 128-sample block on the audio thread and does three things per sample:

1. **LUFS:** K-weight each channel, square, channel-sum: `kw = kl² (+ kr²)` (`meter.cpp:61-64`), accumulate into a 100 ms sub-block sum.
2. **Peak:** decay then max: `peak_lin_ *= peak_decay_per_sample_; if (a > peak_lin_) peak_lin_ = a` (`meter.cpp:66-70`). The decay is `exp(−1/(0.375·sr))` per sample (`meter.cpp:46-47`) — τ ≈ 375 ms, ≈ 11.6 dB/s of fallback, so the display holds a transient long enough to see, then relaxes.
3. **RMS:** square the *channel average* `m = 0.5·(L+R)` (`meter.cpp:72-74`) into its own 100 ms sub-block.

On each 100 ms boundary the sub-block mean-square is committed into a ring with an **incrementally maintained running sum** — `ring_sum_ += ms − ms_ring_[ring_idx_]` (`meter.cpp:76-81`) — so the sliding-window mean is O(1) per update, never a re-sum of 30 blocks. Then:

- **Short-term** = mean over all filled sub-blocks, up to `kShortMs/kSubMs = 30` (3 s) — `meter.cpp:84`. `ring_filled_` ramps from 0 after reset, so the meter produces sensible readings within the first 100 ms instead of waiting 3 s (the effective window just starts short).
- **Momentary** = mean over the newest `kMomBlocks = 4` sub-blocks (400 ms) — `meter.cpp:87-94`, walking backward from `ring_idx_` (which points one past the newest entry).
- **RMS** uses its own 3-slot ring (`rms_n_ = 3`, `meter.cpp:40`) → a ~300 ms window, converted with power-domain `10·log10` (`meter.cpp:107`).

Mean-square becomes LUFS via the standard formula (`k_weighting.hpp:62-65`):

```
L_K = −0.691 + 10·log10(mean_square)      (−120 floor for silence)
```

`−0.691` is BS.1770's absolute calibration offset; it's `10·log10` (not 20·) because the argument is already power. `readout()` (`meter.cpp:114-122`) converts the peak with the *amplitude* formula `20·log10` — the one place the 10-vs-20 distinction bites people.

Note what this meter is **not**: it computes sliding means, not the BS.1770 *gated integrated* program loudness (the −70 LUFS absolute / −10 LU relative gate). It's a live monitoring meter, not a substitute for an offline integrated-LUFS measurement of a bounce. And the peak is a **sample** peak — inter-sample (true-peak) protection is the `TruePeakCeiling` stage's job, not the meter's.

### Wiring and publication

Two instances live on the `Plugin` (`axon_plugin.cpp:788-790`): `meter_in` taps the raw pre-chain input at the top of `flush_chain_block_` (`axon_plugin.cpp:1511-1516`), `meter_out` taps `out_buf` after Trim + TruePeakCeiling (`:2132-2135`) — the real master (§4 explains why *there*). Both readouts are published through eight relaxed `std::atomic<float>`s (`:791-794`, stores at `:2136-2147`); the main thread formats them into a single `axonMeters({...})` JS call for the WebView at the same ~21 fps cadence as the spectrum (`:2633-2648`). Each atomic is independent, so a reader can see fields from two adjacent blocks — a benign meter race, deliberately tolerated. `reset()` is the only allocating call (`meter.cpp:22-51`); it runs at activate and on `plugin_reset` (`axon_plugin.cpp:1239-1240`, `:1290-1291`).

---

## 2. Auto Gain: feed-forward, not feedback

`AutoGain` (`auto_gain.hpp`, ~70 lines) drives the *delivered* output loudness toward the *input* loudness so that engaging the chain is loudness-neutral. Toggle Bypass with Auto Gain on and the only thing that changes is the processing — not the volume. It has one user control: the AUTO GAIN button (`AGN`, resolved at `axon_plugin.cpp:1425`/`:1470`; UI button `ui/index.html:470`, tooltip "Match output loudness to input for fair A/B").

### The design story: why not a feedback loop

The obvious first cut — and the one that was actually built and rejected — is a feedback integrator: measure the loudness of what you output *after* your own trim, compare to the input, nudge the trim. That's a closed loop around a 3-second measurement window: the quantity you measure depends on the gain you chose, so the loop rings, overshoots, and needs loop-stability tuning for what should be a bookkeeping feature.

The shipped design cuts the loop open. The OUT meter reads the **real, uncompensated master** — the tap is upstream of Auto Gain's multiply — so the measured offset is **independent of the gain Auto Gain applies**. The target is then just:

```
target = clamp(in_lufs − out_lufs, ±24 dB)        (auto_gain.hpp:53)
```

and the running gain eases toward it with a one-pole smoother in dB:

```
g_db_ += (target − g_db_) · kSmooth               (kSmooth = 0.004/block, auto_gain.hpp:54)
```

No loop dynamics: the target doesn't move because the gain moved. At 48 kHz a 128-sample block is ≈ 2.67 ms, so τ ≈ 2.67 ms / 0.004 ≈ **0.67 s**, ~95 % settled in ~2 s — deliberately matched to the 3 s short-term LUFS window it consumes. Faster would pump; slower would lag audibly.

### The `ff_db` trick: known gains are cancelled in one block

The slow loop has one ugly consequence: grab the Mel limiter's **Drive** knob (+6 dB of known, deterministic input gain) and the monitor level would bump for ~2–3 s while LUFS catches up. So the call site computes the *known* part of the loudness change and feeds it forward:

```cpp
const float drive_db = 20.f * std::log10(std::max(1e-6f, amt.ml_drive_lin));
const float ff_db    = drive_db * amt.ml_wet;          // axon_plugin.cpp:2161-2162
```

and `AutoGain` injects only the **per-call delta**:

```cpp
g_db_ += -(ff_db - last_ff_db_);   last_ff_db_ = ff_db;    // auto_gain.hpp:47-48
```

A +6 dB Drive jump is cancelled in the *same* block; the LUFS loop then trims the small nonlinear residual the limiter actually produced (limiting isn't a pure gain). Because the loop always converges to `target` regardless, the injection only shapes the transient — it cannot bias the steady state. Two subtleties in the disabled branch (`auto_gain.hpp:36-42`): the gain *relaxes* to unity (`g_db_ *= kRelease`, 0.995/block) rather than snapping (no click), and `last_ff_db_` keeps tracking while off, so re-enabling doesn't dump in all the Drive movement that happened meanwhile.

Guards: a **silence gate** (`kFloor = −50` LUFS, both meters must be above it, `auto_gain.hpp:52`) prevents silent passages — where LUFS sits at the floor — from winding the gain to the clamp; and the `±24 dB` clamp (`:56`) bounds any pathological reading.

### The tap-order contract

The three lines of ordering in `flush_chain_block_` are the load-bearing part (`axon_plugin.cpp:2111-2175`):

1. Trim + `TruePeakCeiling` finish the **real master** into `out_buf` (`:2117-2126`).
2. `meter_out` reads it (`:2132-2135`) — so driving the limiter *visibly raises* OUT LUFS. The meter never lies about the master you'd print.
3. `auto_gain.process(...)` computes the trim from those real readings and multiplies `out_buf` **after** the tap (`:2163-2172`).

So the OUT meter and your ears legitimately disagree when Auto Gain is on — the meter shows the loud master, you hear the matched one. That's the feature. It also means Auto Gain is **monitoring-only by placement, not by code path**: if you bounce with it enabled, the trim *is* in the delivered audio — hence the source comment "render with Auto Gain off to print the loud master" (`:2157`).

### Tests

`tests/test_auto_gain.cpp` locks the behavior with a tiny model harness (`converge()`, `:16-21`) that feeds `out = in + proc_db` — the feed-forward assumption stated as code. It verifies: convergence to −6 dB against a +6 dB chain and +4 dB against a −4 dB chain (±0.5 dB); relaxation to exactly 0 dB when disabled; the silence gate holding unity; the clamp pinning at −24 dB against an absurd +60 dB chain; and — the characteristic test — `test_feed_forward_instant` (`:64-77`): after settling matched, a +6 dB `ff_db` jump with an *unchanged* out meter must drop the gain by ~6 dB **in one call**.

---

## 3. The oracle pattern: LufsLeveler as ancestor and independent referee

`LufsLeveler` (`lufs_leveler.{hpp,cpp}`) is not in the chain. It's the conceptual **ancestor** of the meter + Auto Gain pair: a self-contained BS.1770-4 short-term LUFS meter fused to an attack/release gain rider that pins audio at an absolute target (default −14 LUFS, `Config`, `lufs_leveler.hpp:28-38`). In the early architecture it powered explicit Input/Output Leveler stages. It was removed from the chain because it entangled measurement with gain control (the product wanted a *visible* meter and an *honest A/B*, not a black-box normalizer), because absolute-target leveling in the path fights both the user's loudness decisions and the downstream limiter, and because open-loop ±12 dB of gain with no look-ahead is a clipping risk inside a mastering chain. Its two halves became `LoudnessMeter` (measurement, generalized with momentary/RMS/peak) and `AutoGain` (gain riding, scoped down from "hit −14" to "match the input").

But it was kept, compiled, and tested — because a from-scratch, dependency-free second implementation of BS.1770 is exactly what you want to cross-check the production meter against. This is the **oracle pattern**: `test_meter` links *both* units (`native/clap/CMakeLists.txt:154-155`) and asserts they agree on identical signals.

`test_lufs_matches_leveler` (`tests/test_meter.cpp:39-77`) runs 5 s of pink noise through both and compares `LoudnessMeter::readout().lufs_s` against `LufsLeveler::last_measured_lufs()` — mono and decorrelated stereo (via `process_linked`, which uses the BS.1770 channel-sum L²+R² and one linked gain ride) — to within 0.5 LU. The two implementations share only the coefficient tables in `k_weighting.hpp`; the biquad state handling, sub-block accumulation, ring bookkeeping, and window ramp-up logic are written independently on each side, so the comparison genuinely exercises the meter's machinery. The rest of the file pins absolutes and regressions:

- scaling: ×2 amplitude ⇒ +6.02 dB on LUFS, RMS, *and* peak (`:79-101`);
- a −6.02 dBFS sine reads peak −6.02, RMS −9.03 (`:103-121`); silence floors everywhere (`:123-138`);
- leveler-driven-to-−14 pink noise meters at −14 ± 1.5 (`:140-158`) — the two units validating each other in the mastering-relevant regime;
- the division-by-zero guards: `reset(0.0)` on the leveler must behave identically to a correctly-initialized 48 kHz instance (`:174-231`), a zero short-term window must not modulo-by-zero (`:233-266`), and — meter-specific — `LoudnessMeter::reset(0.0)` must not produce NaN LUFS from Inf warp coefficients (`:287-316`), with a companion test proving the guard leaves the valid-rate path untouched by re-running the oracle cross-check at 48 kHz (`:318-335`).

The leveler also remains a live probe in `test_dsp.cpp` (silence-gate/convergence/attenuation tests at `:60-122`, target `CMakeLists.txt:171`). Its DSP details — 50/500 ms attack/release poles, ±12 dB clamps, the −70 dBFS silence gate that freezes the target instead of boosting fades, the per-*sample-pair* stereo mean-square normalization — are all still accurate in the source and worth reading (`lufs_leveler.cpp:72-118` mono, `:120-166` linked), but they describe a reference implementation, not shipping behavior. This dive replaces `docs/lufs_leveler.md` as its documentation of record.

---

## 4. The Bypass FIFO: time-aligned A/B by construction

The internal **Bypass** button (`BYP`, `ui/index.html:471` — "Audition the raw input (level-aligned, no DAW bypass)") is the other half of honest A/B. A DAW's own bypass un-delays the plugin, so toggling it shifts the audio in time by the plugin latency — a click, a comb, or at minimum a disorienting jump. Axon instead auditions the dry signal through a delay line that lags by *exactly* the plugin latency, with zero explicit latency math:

- **Push:** as host input is consumed into the 128-sample chain accumulator, the same raw samples are mirrored into a per-channel ring `bypass_fifo` (`axon_plugin.cpp:2385-2390`; ring declared at `:662-666`, size `kBypassRing = 32768` at `:707`).
- **Pop:** as processed output drains from the output ring, the dry read index advances **in lock-step — always, even when Bypass is off** (`:2352-2364`). Bypass merely *selects* the dry sample over the wet one:

```cpp
const float wet = plug->chains[ch].out_buf[plug->chains[ch].out_read];
out_ch[ch][out_pos] = amt.bypass_on ? plug->chains[ch].bypass_fifo[rd] : wet;
```

Because pushes happen at consumption and pops at delivery, the dry stream *naturally* trails the wet stream by however many samples are in flight — which is precisely the latency the plugin reports to the host. `compute_latency_` (`:934-979`) sums the sources: the 128-sample accumulator (always), the ceiling lookahead (always), the spectral-mask EQ's `n_fft` when that renderer is active, `kSslHop − kBlockSize = 896` when the bus comp is wet, and the Mel limiter's 1280 when it's wet. Whatever that adds up to for the current parameter state, the FIFO is aligned to it *by construction* — no recomputation on parameter changes, no resync logic; the 32 768-sample ring is simply comfortably larger than any achievable total. Toggling Bypass is therefore phase-coherent: same moment in the music, dry vs wet.

Two consequences complete the honesty story. The dry path gets **neither** the Trim, the ceiling, nor the Auto Gain multiply (Auto Gain is applied inside `flush_chain_block_` to `out_buf` — the wet path — before the drain loop selects), so Bypass plays the true input at its true level. And since Auto Gain has matched the *wet* path to that same input loudness, the A/B is simultaneously **time-aligned and loudness-matched** — the two toggles compose. The write/read indices are shared across channels and advance in lock-step (`:705-708`); both are re-zeroed and the ring cleared on activate and reset (`:1228-1230`, `:1303-1317`).

---

## 5. The SpectrumAnalyzer: Goertzel per stage, try-lock hand-off, EMA display

`SpectrumAnalyzer` (`axon_plugin.cpp:128-304`) renders one spectrum per **chain position** — the output of every reorderable stage — so the UI can overlay them and show which stage did what. Since the exciter's removal there are **`kNumStages = 7`** positions (`:100`), default order BassMono → SslEq → AutoEQ → Reverb → Widener → SslComp → MelLimiter (`kDefaultStageOrder{6,3,1,8,9,4,5}`, `:125`; StageID 7 is a retired value gap so saved sessions never re-key, `:113-114`). Older docs describing 5 stages and `{1,2,4,6,5}` are stale. Position index — not stage ID — keys the accumulators; `build_js` ships `processor_order` alongside the data so the UI maps curve → stage (`:250-251`).

### Producer: the audio thread accumulates, never blocks

After every stage's switch case, unconditionally:

```cpp
plug.spectrum.push(pos, work_l, work_r, n_ch, kBlockSize);   // axon_plugin.cpp:2101
```

`push` (`:182-193`) appends up to `kFFT − fill` samples of the **mono average** `0.5·(L+R)` into that position's 2048-sample accumulator and ignores overflow. The AutoEQ stage additionally latches its current EQ curves for the overlay — a 5-point set at historical PEQ centres (`set_eq_gains`, `:196-198`) and the 50-point log-spaced `set_eq_bins` (`:200-203`) — sampled from whichever renderer is active, IIR bank (`:1712-1721`) or STFT mask (`:1732-1740`).

When position 0's accumulator fills — every 16 blocks — `advance_and_transfer` (`:208-220`) runs the hand-off: **`try_lock`** on `xfer_mtx`; on success copy all 7 frames plus the EQ-gain/bin snapshots into the `xfer_*` staging and set `xfer_ready`; on contention skip — that frame's spectra are dropped, which is fine for a visualizer. Either way all fills reset and the caller wakes the main thread via `host->request_callback` (`:2108-2109`). The audio thread never waits: this is the standard snapshot-under-try-lock SPSC pattern, wait-free on the producer side.

### Consumer: Goertzel at exactly the bins you draw

`process_if_ready` (`:223-244`) copies `xfer_* → mt_*` under a brief `lock_guard` (memcpy only — all DSP happens after release), then, per position, Hann-windows the frame and evaluates **`kDisp = 128` log-spaced display bins** from 20 Hz to 20 kHz (`disp_hz[i] = 20·1000^(i/127)`, `:176-177`).

Why Goertzel instead of an FFT? An FFT computes all 1024 linearly-spaced bins; the display wants 128 *log-spaced* points that don't sit on the FFT grid anyway. The Goertzel algorithm (`:291-303`) computes the DFT magnitude at a single bin with a two-multiply recurrence:

```
coeff = 2·cos(2πk/N)
s[n]  = x[n] + coeff·s[n−1] − s[n−2]        n = 0…N−1
power = s1² + s2² − coeff·s1·s2             magnitude = √power · (2/N)
```

Cost is O(K·N) = 128 × 2048 per stage — ~1.3 M inner iterations per frame across 7 stages, ~21×/s, on the **main** thread, far from any audio deadline. Two honest caveats baked into the implementation: the fractional target bin `disp_hz[b]·kFFT/sr` is **rounded** to the nearest integer bin and clamped to [0, N/2] (`:292-293`), so with the ~21.5 Hz grid at 44.1 kHz several adjacent display points below ~100 Hz quantize to the same bin; and the `2/N` scaling doesn't compensate the Hann window's 0.5 coherent gain, so the dB axis is *relative*, not calibrated.

Each magnitude feeds a one-pole EMA, `ema = 0.65·ema + 0.35·mag` (`kAlpha`, `:240`) — at the ~21 fps frame cadence (2048/44100 ≈ 46 ms) that's a ≈ 110 ms display time constant: readable, not seizure-inducing, still responsive. `build_js` (`:247-287`) serializes order, the 7×128 dB matrix (`20·log10(max(ema,1e-9))`), the 5-point `eq` overlay, and `eq_bins` (or `null`) into one `axonSpectrum({...})` call.

### What's new since the old doc: the SSL overlay and the decimation

The UI now draws a **complete EQ decomposition** — the auto-EQ's curve (`eq_bins`), the SSL EQ's total contribution, and their interaction. The SSL curve comes from a main-thread-only scratch instance, `ssl_viz_eq` (`:786`, prepared at `:1236`): `plugin_on_main_thread` loads the current manual band params plus the seqlock-published coupling assist gains into it and samples `magnitude_db()` at the same 50 log bins, shipping `axonSslCurve({...})` (`:2597-2631`). Audio-thread SSL state is never touched for display.

And the overlay evaluation got the perf treatment. Channel 0 was evaluating 55 magnitude-response points across a 64-biquad cascade **every 128-sample flush**, while the spectrum transfer only consumes the latch every 16th flush — 15 of 16 evaluations overwritten unseen, GUI or no GUI. The shipped fix (`perf_stage_ranking.md` #4) gates it to every 8th flush (`eval_disp`, `axon_plugin.cpp:1697-1704`): still ≤ ~23 ms stale, comfortably ahead of the ~46 ms UI cadence, audio path bit-identical. **Measured: AutoEQ 150.8 → 132.6 µs/block.** Why every 8th and not 16th? Headroom: the latch is asynchronous to the transfer, so double-rate evaluation bounds staleness at half a UI frame instead of a full one.

---

## 6. The plot twist: the visualizer is a data bus

Here is the part that makes the spectrum analyzer *not* just chrome. The auto-EQ → SSL "calibration" feature (SEQ_CAL) fits the four real SSL bands to the auto-EQ's current correction curve — and the curve it fits against is **`spectrum.mt_eq_bins`**, the main-thread copy of the visualizer's 50-point EQ latch (`solve_ssl_coupling_`, `axon_plugin.cpp:2207-2256`, target built at `:2230-2235`). The spectrum transport — audio-thread latch, try-lock snapshot, main-thread copy — is the *only* path by which the auto-EQ's resolved curve reaches the solver.

Which is why the pump runs unconditionally:

```cpp
// Pump the spectrum transport regardless of GUI so the SSL coupling solve runs
// headless (the assist-band fit needs a fresh auto-EQ curve every ~1 s).
const bool spec_ready = plug->spectrum.process_if_ready(plug->sample_rate);
solve_ssl_coupling_(*plug, spec_ready);
if (!plug->gui_state) return;                        // axon_plugin.cpp:2586-2591
```

With no editor open, the Goertzel/EMA work still runs and the EQ-bin snapshot still lands in `mt_eq_bins`, because a DSP feature depends on it. The solver's results flow back through two channels: the solved band gains are written onto the **visible** SEQ_*_G knobs via the GUI→audio param queue (`set_visible_param_`, `:2188-2197` — the knobs are the integrator), and the dormant 6-band assist layer publishes through a **seqlock** (`ssl_asg_gen` even = stable; writer increments to odd, writes, increments to even, `:2217-2221`; audio-thread readers retry on odd/changed generations with acquire fences, `:1539-1547`).

The consequences deserve stating plainly, because they're the kind of thing a refactor silently breaks:

- `SpectrumAnalyzer::kNumBins`, the 20 Hz–20 kHz log spacing, and the `set_eq_bins` latch are **part of a DSP contract**, not display tuning. The solver rebuilds the identical frequency grid independently (`:2232-2233`); change one side and the fit quietly targets the wrong frequencies.
- The overlay-decimation gate (§5) is also bounding the *solver's* input staleness, not just the UI's.
- "Remove the spectrum analyzer when the GUI is closed" is not an available optimization.

The visualizer earned its keep by being honest; it kept its job by becoming infrastructure.

---

## 7. Performance: what the whole stack costs

From the instrumented ranking (`native/clap/docs/perf_stage_ranking.md`, buf=128, all-stages, 68 900 blocks):

| Unit | share of process() | mean µs/block | notes |
|---|---:|---:|---|
| MeterIn | 0.253 % | 1.14 | 2 biquads + ring bookkeeping, 128 samples ×2 ch |
| MeterOut | 0.252 % | 1.14 | identical work on the master |
| SpectrumPush | 0.027 % | 0.018 | 7 taps/block; the 2048-sample copy amortizes to ~nothing |
| AutoGain | 0.011 % | 0.048 | one log10, one pow, one buffer multiply |
| **Total** | **≈ 0.54 %** | ≈ 2.4 | filed under "explicitly cheap — leave alone" (§5 of the ranking) |

The heavy lifting (640→896 Goertzel runs per frame after the stage count grew to 7) is main-thread and therefore not in this budget at all. The one monitoring-adjacent perf item that *was* worth shipping was the display-curve decimation (§5): it lived in the AutoEQ stage's audio-thread time, not the analyzer's, which is exactly why it was found by per-stage timing rather than by profiling the analyzer.

---

## 8. Honest limitations and open edges

- **No integrated/gated LUFS.** Both meters are sliding means; there is no −70 LUFS absolute / −10 LU relative gate and no whole-program integration. Long silences drag short-term LUFS down (the *display*; Auto Gain is protected by its own −50 gate). An offline render measurement is still the authority for delivery specs.
- **Sample peak, not true peak.** `peak_db` doesn't oversample; inter-sample peaks under-read. The ceiling stage handles actual true-peak protection — the meter just shouldn't be mistaken for a TP meter.
- **RMS phase sensitivity.** RMS averages `0.5·(L+R)` before squaring, so anti-phase content cancels and reads low. LUFS (L²+R²) doesn't. Intentional (that's what "mono-sum RMS" means) but occasionally surprising.
- **Off-rate K-weighting is a warp, not a redesign.** At rates other than 44.1/48 kHz the proportional z-plane scaling is an approximation; fine for monitoring and matching, not metering-grade at exotic rates.
- **Auto Gain matches loudness, not perception.** Two signals at equal short-term LUFS can still differ in perceived loudness (spectral tilt, crest). LUFS matching removes the dominant bias, not every bias. Only *known* gains get the `ff_db` fast path — EQ/saturation/compression moves glide over ~2 s by design.
- **Spectrum accuracy is bounded by design choices:** integer-bin snapping quantizes the lowest octaves (several display points share one 21.5 Hz-grid bin); the dB axis is uncalibrated (Hann coherent gain uncompensated); stereo is folded to mono; frames drop silently under lock contention. All fine for the purpose — but the same `mt_eq_bins` path now feeds a solver, so "it's just a visualizer" is no longer a valid dismissal for its latching/staleness semantics (the solver tolerates ~1 s staleness today; see §6).
- **A `try_lock` still touches a mutex on the audio thread.** Non-blocking, once per 2048 samples — safe in practice, but it's the first thing to revisit if RT guarantees ever tighten (a seqlock'd triple buffer would be the drop-in).
- **Meter publication races are benign but real.** Relaxed atomics per field mean a UI frame can mix two blocks' values; the bus-comp/limiter telemetry uses plain floats with the same shrug. Documented, deliberate, cheap — just don't build anything decision-making on top of the UI-side values.
- **The Bypass FIFO trusts lock-step invariants.** Shared `bypass_w/bypass_r` across channels assume all channels fill and drain identically (they do — one shared `in_pos/out_pos` drives them, `axon_plugin.cpp:2348-2349`), and 32 768 samples must exceed worst-case latency (it does, by ~7×). Neither is enforced by assertion.
- **Open ideas:** a true-peak readout on the existing meter (the ceiling already computes oversampled peaks — plumbing, not DSP); gated integrated LUFS for a "session loudness" display; fractional-bin Goertzel to fix the bass quantization; formalizing the spectrum→solver contract (shared constant for the 50-bin grid instead of two independent constructions).

---

## Supersedes / source map

This dive replaces **`docs/loudness_meter.md`** (content carried over; its `meter.cpp:17-24` coefficient anchors drifted when the constants moved to `k_weighting.hpp`, and its plugin anchors predate the current file layout), **`docs/spectrum_analyzer.md`** (Goertzel/EMA/hand-off walkthrough carried over; stale on stage count/order, the exciter removal, the SSL decomposition overlay, the headless pump, and the display-curve decimation), **`docs/auto_gain.md`** (accurate; folded in with updated anchors), and **`docs/lufs_leveler.md`** (folded to its actual role: ancestor + oracle).

| Topic | Where |
|---|---|
| Meter DSP | `meter.hpp:26` (Readout), `:58-60` (windows); `meter.cpp:10-20` (K-weighting load + sr guard), `:22-51` (reset), `:53-112` (process), `:114-122` (readout) |
| Shared BS.1770 constants | `k_weighting.hpp:28-40` (tables), `:45-59` (selection + warp), `:62-65` (`lufs_from_ms`) |
| Meter wiring | `axon_plugin.cpp:788-794` (instances + atomics), `:1511-1516` (IN tap), `:2132-2147` (OUT tap + publish), `:2633-2648` (`axonMeters` JS) |
| Auto Gain | `auto_gain.hpp:34-58` (process), `:47-48` (ff delta), `:63-66` (constants); call site `axon_plugin.cpp:2153-2175`; UI `ui/index.html:470`, `:2043` |
| Bypass FIFO | `axon_plugin.cpp:662-666`, `:705-708`, push `:2385-2390`, pop/select `:2352-2364`; latency `:934-979` |
| Spectrum | `axon_plugin.cpp:128-304` (unit), taps `:2100-2109`, overlay latch + decimation `:1688-1740`, pump `:2586-2595`, SSL viz curve `:2597-2631` |
| Spectrum-as-bus | `axon_plugin.cpp:2207-2256` (solver reads `mt_eq_bins` `:2234`), seqlock write `:2217-2221` / read `:1539-1547` |
| Oracle | `lufs_leveler.{hpp,cpp}`; `tests/test_meter.cpp` (cross-checks `:39-77`, `:318-335`; guards `:174-316`); `tests/test_auto_gain.cpp`; targets `CMakeLists.txt:154-171` |
| Measured perf | `native/clap/docs/perf_stage_ranking.md` (§2 table, §5 cheap-stages, §6 shipped #4: AutoEQ 150.8 → 132.6 µs/block) |
