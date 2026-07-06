# Bus Comp: streaming a causal TCN on the audio thread, and shrinking it 20% with byte-identical graph surgery

> The Bus Comp stage runs a trained neural network — a causal Temporal
> Convolutional Network that learned the behaviour of an SSL-style bus
> compressor — inside the audio callback, in real time, with a hard deadline of
> a few milliseconds per block. This doc explains how that actually works (the
> ring/hop/receptive-field contract, the dry-path alignment that prevents comb
> flutter, the level-matched input drive, the coherence-based "crunch" meter),
> and then tells the optimization story that capped it: the model was doing
> 27.8% throwaway work per inference, a falsifiable experiment proved it, and
> in-place ONNX graph surgery removed it — **byte-identical at three levels of
> proof**, no retraining, forward cost down 20.5%.
>
> Code: `native/clap/src/axon_plugin.cpp` (the `SslComp` stage + `OrtMiniSession`),
> `native/clap/src/coherence_distortion.hpp`, `native/clap/src/axon_limits.hpp`.
> Model: `weights/axon_bundle/ssl_comp/{model.onnx,plugin_meta.json}`.
> All line references are to `origin/main`.

---

## 1. The problem, and what is unusual about the solution

Bus compression is the "glue" of a mix: program-dependent attack and release,
subtle harmonic colour, behaviour that lives in *how* the unit reacts rather
than in any textbook gain-computer equation. Instead of hand-writing that
curve, Axon trained a network on (input, output) audio pairs from the target
compressor and ships the trained function. At runtime there is no Python and
no training code — just ONNX Runtime (ORT) executing a fixed graph of
convolutions on the audio thread.

Two things about this stage are worth a deep dive:

1. **The streaming contract is visible in plain sight.** The whole mechanism —
   how a network that needs 631 samples of past context produces gap-free
   audio in 128-sample blocks — reduces to one inequality between three
   integers, asserted at activate and pinned by a unit test.
2. **The optimization is provably free.** Most DSP perf work trades accuracy
   for speed and argues about null-test floors. Here, the claim "the model
   computes 27.8% output it never uses" was tested by perturbing inputs and
   checking output *bytes*; the fix was surgery on the ONNX graph itself; and
   the result was proven byte-identical at the graph level, the model level,
   and the full-plugin-render level. "Bit-identical" is not a figure of speech
   anywhere in this story.

Where it sits in the default chain (stages are user-reorderable;
`axon_plugin.cpp:711`):

```
input ─▶ BassMono ─▶ SslEq ─▶ AutoEQ ─▶ Reverb ─▶ Widener ─▶ SslComp ─▶ MelLimiter ─▶ TruePeakCeiling ─▶ trim ─▶ out
                              (neural LSTM ctrl)             (neural TCN — this doc)
```

---

## 2. The model: a 631-sample causal TCN

The architecture is fixed in `conf/model/tcn/model_bb_tcn_ssl_comp.yaml`:
a causal TCN with 6 residual blocks, kernel size 11, dilations 1, 2, 4, 8,
16, 32, channel width 16, tanh activations, ~10K parameters (~69 KB ONNX).
The receptive field follows directly from the dilation stack:

```
rf = 1 + (kernel-1) · Σ dilations = 1 + 10 · (1+2+4+8+16+32) = 631 samples  ≈ 14.3 ms @ 44.1 kHz
```

Each output sample depends on exactly the current input sample and the 630
before it — **causal** (never a future sample) and **stateless** (no recurrent
state carried between calls; the context window *is* the state). The yaml's
header comment explains why 14 ms of memory suffices: the reference unit was
captured in peak-catcher mode with a ~1 ms release, so the behaviour is "catch
peak, drop gain for the peak's duration, snap back" — there is no long-term
auto-release envelope to model.

The shipped `weights/axon_bundle/ssl_comp/plugin_meta.json` declares the
runtime contract:

| Field | Value | Meaning |
|---|---|---|
| `architecture` | `"tcn"` | streaming TCN, run live in ORT |
| `stage_kind` | `"nn"` | pure network — no `dsp_blocks` (cross-checked in `meta.cpp:130-155`) |
| `causal` | `true` | output at *t* depends only on inputs ≤ *t* |
| `receptive_field` | `631` | past samples per output sample |
| `trace_len` | **`1655`** | the static input length the ONNX accepts (was 2048 — see §8) |
| `num_controls` | `0` | non-parametric: no knob tensors into the graph |
| `state_tensors` | `[]` | stateless |
| `sample_rate` | `44100` | the plugin refuses to activate at any other rate |

`trace_len` parses with a legacy default of 0 (`meta.cpp:89`, field at
`meta.hpp:87`), and the SslComp activate path only engages when it is
positive (`axon_plugin.cpp:1178`).

---

## 3. The streaming export contract

The contract between the Python exporter and the C++ runtime is small and
strict, and the code states it verbatim (`axon_plugin.cpp:1834-1838`):

> the ONNX takes `trace_len` samples in (the entire ring, including the `rf-1`
> history prefix) and produces `trace_len - (rf-1)` samples out — the model's
> predictions for ring positions `[rf-1, trace_len-1]`. Output position i
> corresponds to ring position (rf-1 + i).

The single most important equation:

```
actual_olen = trace_len − (receptive_field − 1) = 1655 − 630 = 1025
```

The export has **no internal pre-padding**: it *trims* the first `rf−1`
positions instead of zero-padding them, so a length-N input yields a length
`N−(rf−1)` output and every output sample has a genuine, fully-populated
receptive field. With the shipped numbers:

```
ring buffer (trace_len = 1655 samples)
┌──────────────────────────┬─────────────────────────────────────────┐
│ history / "warmup"       │ valid predictions                        │
│ positions [0 .. 629]     │ positions [630 .. 1654]                  │
│ (rf−1 = 630 samples)     │ (1655 − 630 = 1025 samples)              │
└──────────────────────────┴─────────────────────────────────────────┘
                            ^ first sample with a full receptive field
ORT output length = 1025;  the plugin consumes the LAST kSslHop = 1024
```

**Analogy:** reading with a 631-word memory — you can only "understand" a word
once you have seen the 630 before it; the head of the page is context you
needed to get going, not output.

The input shape is **static**: `audio_in` is `[1,1,1655]` float32, and ORT
rejects any other length with `INVALID_ARGUMENT` (verified — §8.2). All sizing
downstream of the JSON is data-driven: ring and output buffers are allocated
from `trace_len` at activate (`axon_plugin.cpp:1196-1197`), `actual_olen` is
computed per block (`:1853`), and the consumed slice offset is
`actual_olen − kSslHop` (`:1938`). Changing the model's size is a
bundle-only change; no C++ constant knows "1655".

---

## 4. The hop machinery

### 4.1 Why a hop at all

The plugin flushes its chain in fixed `kBlockSize = 128` sample blocks
(`axon_limits.hpp:18`; host audio is accumulated into 128-sample blocks at
`axon_plugin.cpp:2376-2408`, which is one block of always-present latency).
Running a full 1655-sample TCN forward every 128 samples would burn ~13× the
compute for the same audio. Instead the stage **accumulates a hop**:

```
constexpr int kSslHop = 1024;   // axon_limits.hpp:33 — one ORT call per 1024 input samples
static_assert(kSslHop % kBlockSize == 0);   // :39 — whole blocks per hop, or the dry/wet alignment silently desyncs
```

Per channel, per 128-sample flush (`case StageID::SslComp`,
`axon_plugin.cpp:1826-2036`):

```
accum[fill .. fill+127] ← input block          (:1896-1906, with SSC_IN trim — §5)
fill += 128
if fill == kSslHop (every 8th flush):          (:1911)
    memmove ring left by kSslHop               (:1912-1914)  drop oldest 1024, keep newest 631
    copy accum → ring tail                     (:1915-1916)  ring = [631 kept | 1024 new]
    ORT run(ring[1655]) → obuf[1025]           (:1925-1927)
    outq ← last kSslHop of obuf                (:1938-1940)  = predictions for the 1024 new samples
pop 128 samples from outq → wet                (:1968-1971)
blend wet against the TIME-ALIGNED dry         (:2002-2003)
```

The output slice arithmetic (comment at `:1933-1937`): output sample *i* is
the prediction for ring position `rf−1+i`, so the newest `kSslHop` ring
positions `[N−1024, N−1]` live at output positions
`[N−1024−(rf−1), actual_olen−1]` — the **last 1024** of the 1025 outputs. The
1024 stashed predictions then drain over the next `kSslHop / kBlockSize = 8`
host flushes.

### 4.2 The load-bearing inequality

Every hop shift discards `kSslHop` samples of history. For the *oldest new*
output sample to still see its full receptive field, the ring must retain at
least `rf` samples of past context after the shift:

```
kSslHop ≤ trace_len − rf          (1024 ≤ 1655 − 631 = 1024   ✓  margin: exactly 0)
```

Violate it and the newest outputs are computed against a truncated context —
a discontinuity at every hop, i.e. audible flutter at 44100/1024 ≈ 43 Hz.
The constraint is enforced twice:

- **At activate** (`axon_plugin.cpp:1184-1190`): throws
  `"ssl_comp: kSslHop=… exceeds trace_len-RF=…"`. Because activate is a C-ABI
  CLAP callback where an escaping exception is `std::terminate` (it crashed
  the DAW before the guard), the impl is wrapped in try/catch and turned into
  a clean activation failure (`:1254-1272`).
- **At test time, against the shipped artifact**
  (`native/clap/tests/test_ssl_hop_contract.cpp`): CMake injects
  `SSL_META_PATH` pointing at the real
  `weights/axon_bundle/ssl_comp/plugin_meta.json`
  (`native/clap/CMakeLists.txt:208-215`), the test includes the same
  `axon_limits.hpp` the plugin compiles, loads the real meta and asserts
  `kSslHop <= trace_len - receptive_field` (`test_ssl_hop_contract.cpp:40-43`),
  printing the margin. Since the resize the margin is **exactly 0** — any
  re-export with a larger rf or shorter trace, or any bump of `kSslHop`, would
  build fine and die at activate in the DAW; this test makes it die in CI
  instead. `composite.py` cannot check this (it doesn't know `kSslHop`), and
  no other test reads the shipped bundle's `trace_len`.

Note the guard is one sample stricter than the mathematical minimum
(`N − rf + 1 ≥ kSslHop` would allow N = 1654). That asymmetry is why the
resized model is 1655, not 1654 — see §8.3.

### 4.3 Latency, and why the dry path is delayed too

The wet output of a hop begins playing in the same flush that completed the
hop: input samples `[0..1023]` go in, and the first wet block emerges while
input block `[896..1023]` is being submitted. So **the wet trails the input by
`kSslHop − kBlockSize = 896 samples`** (~20.3 ms @ 44.1 kHz). Two consequences:

**Reported latency is conditional.** `compute_latency_`
(`axon_plugin.cpp:941-979`) adds the 896 samples only when the stage is
engaged and the bundle is loaded (`:972-973`):

```
kBlockSize (128, always)  +  TruePeakCeiling lookahead (always)
+ n_fft                       (AutoEQ, only when EQ > 0 AND the STFT mask renderer is selected;
                               the default IIR renderer is zero-latency)
+ (kSslHop − kBlockSize)=896  (SslComp, only when SSC > 0 and bundle loaded)
+ MelLimiter::kLatency        (only when MLI > 0)
```

Toggling SSC mid-session re-PDCs the host.

**The dry path must be delayed to match.** The wet/dry blend at `SSC < 1`
would otherwise mix `x[n]` (current dry) against a wet rendering of
`x[n−896]`. Summing a signal with a delayed copy of itself is a comb filter
(notches every `sr/896 ≈ 49 Hz`), and because the wet is re-generated once per
hop the comb's phase relationship shivers at hop rate — the "hop-rate
comb-filter flutter" the block comment warns about (`axon_plugin.cpp:1826-1832`).
The fix is a per-channel **dry delay ring** of exactly `kSslHop − kBlockSize`
samples (allocated at `:1206-1208`): each flush reads the dry written
`kSslHop/kBlockSize − 1 = 7` flushes ago and overwrites in place
(`:1944-1960`), so the blend at `:2002-2003` mixes two views of the *same
absolute-time audio* — the only mix that cannot comb.

### 4.4 Warm-up

After activate the ring is zeros and the output queue is empty. Until the
first hop completes (1024 input samples), `avail < kBlockSize` and the stage
leaves the block untouched — the host hears clean **dry pass-through**, not
silence and not stale-zero delayed dry (`:1962-2006`). Expect ~23 ms before
the comp "engages" after a (de)activate.

---

## 5. SSC_IN: driving a fixed-curve model

The ONNX is non-parametric (`num_controls = 0`) — there is no threshold or
ratio tensor. The model *is* one compressor at one setting; its behaviour
depends entirely on **how hard you drive it**. That is exposed as `SSC_IN`
("Input", −24..+12 dB), injected at load time so bundles that don't declare it
still get it (`axon_plugin.cpp:2756-2772`), read in `resolve_amount_`
(`:1413`) and resolved to linear (`:1452`).

The implementation is a level-matched operating-point trim
(comment at `:1855-1868`, gains at `:1867-1868`):

- `in_gain` multiplies the signal **entering the model's accumulator**
  (`:1900-1905`) — the model sees the boosted/attenuated signal and
  compresses accordingly;
- the **exact reciprocal** `mk_gain = 1/in_gain` is applied to the wet output
  after the queue pop (`:1975-1978`) — undoing the static level change so only
  the *character* (more/less compression) survives;
- the **dry path gets neither gain** — it stays the clean reference for the
  blend and for the coherence meter.

At 0 dB both gains are exactly 1.0 and the code paths are branch-skipped —
bit-for-bit identical to not having the control. One honest caveat, stated in
the source: the input gain and its reciprocal act ~896 samples apart in time,
so *fast automation* of SSC_IN causes a brief level bump; for a static trim it
is exactly correct.

The wet mix itself is `SSC` (a 0/1 switch in the shipped meta) scaled by the
bundle's `amt_ssl_comp.wet_mix_max` (`:1451`, parsed at
`composite_meta.cpp:90`). At `SSC = 0` — or if the optional `ssl_comp`
sub-bundle simply wasn't shipped (`ssl_comp_ort == nullptr`) — the stage
early-outs as a transparent passthrough and parks the telemetry
(`:1842-1850`).

---

## 6. OrtMiniSession: holding ORT on the audio thread

`OrtMiniSession` (`axon_plugin.cpp:386-593`) is the thin wrapper actually
instantiated for the bus comp (one per channel, constructed at activate,
`:1191-1195`) and for the AutoEQ controller classes. The choices that matter:

**Single-threaded, sequential, on purpose** (`:395-399`):

```cpp
opts.SetIntraOpNumThreads(1);
opts.SetInterOpNumThreads(1);
opts.SetExecutionMode(ORT_SEQUENTIAL);
opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
```

ORT must never spawn a thread pool that hands audio-callback work to
non-realtime threads. This is a measured trade-off, not an oversight: with the
real model, 2 intra-op threads take the forward from ~1081 to ~603 µs and 4
threads to ~409 µs (`native/clap/docs/perf_stage_ranking.md`, note 7) — but a
non-RT pool inside the callback risks priority inversion and jitter, so it
stays at 1 (owner decision, standing).

**Stable tensor names.** Input/output names are copied from the meta into
owned `std::string`s, and `"<state>_in"`/`"<state>_out"` are pre-built, so
`run()` never holds a `.c_str()` of a temporary (`:401-417`).

**A defensive output copy.** `run()` wraps the caller's ring in an
`Ort::Value` (no audio copy in), executes, then clamps the copy-out to the
*actual* element count of the output tensor and zero-fills any shortfall
(`:478-495`). A mis-exported bundle whose output length disagrees with
`trace_len − (rf−1)` degrades to silence instead of reading past the tensor —
the comment names the alternative: "garbage memory and a hop-rate flutter on
the wet".

**State machinery, unused here.** The in/out state-buffer maps and
`swap_state()` (`:408-417`, `:510-514`) exist for stateful models; the bus
comp declares `state_tensors: []` and never touches them. The AutoEQ LSTM
controller is the stateful client — since 2026-07-05 it runs a **batch-2**
call (`run_controller`, `:526+`: audio `[2,1,T]`, LSTM state `[2,2,64]`, both
channels in one ORT call). The bus comp is *not* batched: each channel has its
own session and its own forward — which is exactly why the hop spike is a
back-to-back pair (§8.1) and why the stagger idea exists (§10).

There is also a second, general-purpose wrapper in the tree —
`OrtSession` (`ort_session.{hpp,cpp}`), with A/B state banks and a
constructor-time warmup pass — used by the single-model `nablafx_clap` plugin,
not by Axon's chain. Both implement the same export contract.

**Known RT-safety debt:** `run()` still heap-allocates small per-call vectors
and `Ort::Value` wrappers, and ORT's returning `Run()` allocates output
tensors (measured cost: 2.3 µs per ~1000 µs forward — negligible throughput,
real hygiene debt). Tracked with a fix sketch (pre-bound `Ort::IoBinding`
buffers) in `docs/future/not-started/ort_audio_thread_allocations.md`
(issue #18).

---

## 7. The "crunch" meter: coherence, because residuals lie

The UI shows how hard the model is working. That is harder than it sounds: the
TCN has **no readable internal gain-reduction signal** (it's a black box that
emits audio), and it **phase-rotates** the signal — so the obvious proxy,
`rms(wet − dry)`, is pinned near 0 dB even when the model is barely touching
the audio, because phase shift alone produces a huge residual
(`coherence_distortion.hpp:1-17`, member docs at `axon_plugin.cpp:803-818`).

The fix is **magnitude-squared coherence**, which discards phase entirely:

```
γ²(f) = |Sxy(f)|² / (Sxx(f) · Syy(f))
Sxx = ⟨|X|²⟩,  Syy = ⟨|Y|²⟩,  Sxy = ⟨X · conj(Y)⟩,   X = FFT(dry), Y = FFT(wet)
```

γ² → 1 wherever the wet is a *linear* (possibly phase-shifted, possibly
delayed) function of the dry; `1 − γ²` is the fraction of output power **not
linearly explained** — distortion, nonlinearity, time-varying gain: exactly
the "crunch". Critically, coherence is trivially 1 for a single frame; it only
means something when the spectra are averaged across frames, so the class
keeps complex EMAs with a ~200 ms time constant (`kTauSec`, EMA update at
`coherence_distortion.hpp:158-169`).

Implementation (`native/clap/src/coherence_distortion.hpp`, pure DSP, no
CLAP/ORT deps): FFT size 1024, Hann, 50% overlap, Accelerate vDSP `zrip`
real-FFT with the same split-complex packing as `spectral_mask_eq` (bin 0
carries DC in `realp[0]` and Nyquist in `imagp[0]`, `:154-169`; the vDSP ×2
forward scale cancels inside γ², so nothing is normalised). Near-silent frames
are gated on **dry** energy so the floor holds instead of flashing garbage
(`:143-147`); 8 warm-up frames must accumulate before a value is published
(`:172`). The readout is an energy-weighted band average over ~100 Hz–16 kHz,
weighted by output power `Syy` (`:174-191`):

```
distortion = Σ_k Syy[k]·(1−γ²[k]) / Σ_k Syy[k]        bc_distortion_db = 10·log10(distortion)  ∈ [−48, 0] dB
```

The stage feeds it read-only from the blend tap (`axon_plugin.cpp:1869-1878`):
the mono sums `0.5·(L+R)` of the *time-aligned dry* and the *post-makeup wet*
are accumulated across the channel loop (`:1984-1995`) and pushed once per
block (`:2016`). A second proxy rides along without FFTs: **crest reduction**
`crest(dry) − crest(wet)` in dB, clamped ≥ 0 (`:2019-2028`) — dynamics being
squashed. Both are plain float members read by the GUI timer (a benign meter
race, same pattern as the limiter's brick gain) and pushed as
`axonBusComp({...})` (`:2691-2701`). During warm-up, and whenever the stage is
bypassed, the meter parks at the −48 dB floor and the averages reset so
re-enabling starts clean (`:1842-1849`, `:2030-2034`).

The unit tests (`native/clap/tests/test_coherence_distortion.cpp`) pin this
with closed-form known answers rather than tolerances-of-convenience: linear
wet (±dry, 0.5·dry) hits the floor **exactly** (bit-exact γ²=1 path); additive
equal-power noise gives γ² = SNR/(1+SNR) = ½ → −3.01 dB; a hard-clipper
(Bussgang) gives γ² = ¾ → −6.02 dB; and a 4-sample **delay** — the module's
raison d'être — stays ≈ coherent (−37 dB predicted from Hann misalignment)
where the naive residual would scream 0 dB.

---

## 8. The 20% story: measure, prove, cut — without changing a bit

### 8.1 Where the time went

The July 2026 instrumented ranking (`native/clap/docs/perf_stage_ranking.md`,
§2; `AXON_STAGE_TIMING` sub-timer around the forward at
`axon_plugin.cpp:1922-1931`) put the stage in unambiguous first place:

- **SslComp: 58.5% of total process() CPU. Its `SslOrtForward` sub-timer
  alone: 58.0%** — mean **1045 µs/call**, 17,226 calls. The forward *is* the
  stage; ring memmove, dry-delay, telemetry and blends total ~0.7% of it.
- The cost is hop-spiky by construction: both channels' forwards fire
  **back-to-back in the same 128-sample flush** once per 1024-sample hop
  (1 in 8 blocks at buf=128) → a **~2.1 ms spike against the 2902.5 µs
  deadline** at 44.1 kHz/128. Block p95 was 2256 µs; 15 of 68,910 blocks
  missed deadline, all hop-driven; max/mean ≈ 33×.

The suspicious constant: the model was traced at `trace_len = 2048`, so each
forward produced `2048 − 630 = 1418` outputs — of which the plugin consumes
the last 1024. **394/1418 = 27.8% of every forward was computed and
discarded.**

Sidebar — the report that started it: the user's DAW showed "high CPU +
spikes". Diagnosis (memory `project_stage_timing_tooling.md`): the *installed*
build predated a build-cache guard and had been compiled with an empty
`CMAKE_BUILD_TYPE` — effectively -O0 — measuring p50 4.2 ms/block @512 vs
1.4 ms for a proper Release build. The visible "comb" in the DAW's load graph
was real, though: at buffer 512 the hop lands every 2nd block. Two guards now
exist so this cannot recur (build.sh forces Release + timing-off on stale
caches; the installer refuses binaries containing the `axon.stage-timing`
marker).

### 8.2 A falsifiable causality experiment

"The model is causal, so the first 394 outputs don't affect the consumed
range" is exactly the kind of claim that ships a subtle bug if it's merely
believed. `scripts/verify_ssl_comp_model.py` made it falsifiable, running
under the plugin's exact session opts (intra=1, inter=1, sequential,
`ORT_ENABLE_ALL`, CPU EP — `make_session`, `:45-52`), comparing outputs as
**uint32 bit patterns**, not with an epsilon (`changed_indices`, `:59-61`).
Against the original 2048 model:

- **Static shape.** Declared `audio_in [1,1,2048]`; a `[1,1,1654]` input is
  rejected with `INVALID_ARGUMENT … Got: 1654 Expected: 2048`. (No dynamic
  axis to exploit — resizing means editing the graph.)
- **Negative test.** Perturb *every* input in `[0..393]` with 10× unit noise:
  all 1024 consumed outputs **byte-identical**, `max|diff| = 0.0`.
- **Non-vacuity.** The same perturbation *did* change the non-consumed
  outputs — the test can fail, it just doesn't.
- **Boundary scan.** The smallest single input index whose ±10 perturbation
  changes any consumed output is **396**, not 394: the *effective* receptive
  field measures **629 < the declared 631** — two samples of bonus margin
  (the oldest taps are numerically inert).
- **Positive control.** Perturbing only input 1500 changed consumed outputs
  in exactly `[870..1417]` — precisely `[i−(rf−1) .. min(i, last)]` — at
  98.9% density (the gaps are sub-float32-ULP responses).
- **Timing.** Median forward 1.052 ms, p95 1.171 ms standalone — consistent
  with the plugin's ~2.1–2.4 ms two-channel hop blocks.

This licensed the resize with analytic bounds on the saving: between **19.2%**
(input-length scaling, 1−1654/2048) and **27.8%** (output-count scaling,
1−1024/1418), the truth being layer-dependent and in between.

The script survives as a standing gate: its constants now match the shipped
meta (`N = 1655`, `:29-34`), so it re-verifies the *current* model's shape,
causality (the inert prefix is now just index 0), boundary, positive control
and timing on demand — and carries an optional in-memory dynamic-axis probe
(`:198-227`) that re-declares the time dim symbolic and cross-checks
byte-identity of the tail, if the `onnx` package is present.

### 8.3 The graph surgery

Shipped in commit `634f980` (2026-07-05), via **in-place ONNX graph surgery —
no retraining, no checkpoint needed** (the exporter lives in the external
nablafx fork and the original checkpoint location isn't recorded in this
repo; the surgery route made both facts irrelevant).

The obstacle: a naive edit of the input dim (2048 → 1655) fails, because the
graph was not shape-agnostic. Each of the TCN's six residual blocks crops its
skip path with a `Slice` whose `ends` was an **absolute constant baked at
trace time**: `2047, 2037, 2017, 1977, 1897, 1737` — each one "(this tensor's
time length at depth *d*) − 1" under the 2048 trace (the time axis shrinks by
`(kernel−1)·dilation = 10·d` per block: 2048 → 2038 → 2018 → 1978 → 1898 →
1738 → 1418).

The surgery: rewrite all six `ends` to the **length-relative `−1`**. ONNX
`Slice` resolves a negative end against the runtime dimension
(`end + dim_len`), so `−1` denotes "up to the last element of whatever length
arrives" — which at T=2048 resolves to *exactly the old constants*. That makes
the rewrite a mathematical no-op at the traced length, and makes the graph
size-generic. Then set the input time dim to 1655 and the output dim to 1025.

Why 1655 and not the mathematical minimum 1654: the activate guard is
`kSslHop > N − rf` → it requires `N ≥ 1655` (one sample stricter than
`olen ≥ kSslHop` needs — §4.2). At 1655 the shipped guard, and every
meta-derived size downstream, needed **zero plugin code changes**; the cost of
the extra sample is ~0.06%. (Relaxing the guard to admit 1654 was judged not
worth the churn.)

### 8.4 Byte-identical at three levels

The proof chain, every link compared as bytes:

1. **Graph level.** The `−1` rewrite *alone*, still at T=2048, renders
   byte-identically to the original model — 20/20 random seeds. (Isolates the
   surgery from the resize.)
2. **Model level.** The resized model at T=1655, fed the last 1655 samples of
   the same inputs, produces consumed outputs (last 1024) byte-identical to
   the original at T=2048 — 20/20 seeds. (This is the causality result of
   §8.2, now confirmed on the shipped artifact.)
3. **Plugin level.** A full render of the 20 s bench fixture through the old
   bundle vs the new bundle (bus-comp-only params) is **audio-byte-identical**
   end to end — ring warm-up semantics included.

Level 3 matters beyond thoroughness: two *other* stages in this chain
(Auto-EQ, MelLimiter) are documented as ill-conditioned — they amplify 1-ULP
perturbations to −75..−90 dBFS, and ORT renders occasionally flake at that
level run-to-run (`docs/future/not-started/ort_render_nondeterminism.md`,
issue #19). The bus comp being *well-conditioned* is what made a strict
byte-identity bar even available — and the resize met it.

### 8.5 What it bought

| Metric | Before | After | Δ |
|---|---|---|---|
| Standalone forward, median | 0.999 ms | 0.794 ms | **−20.5%** |
| In-chain `SslOrtForward` mean | 1024 µs/call | 824 µs/call | −19.5% |
| Hop-block spike (2-ch pair) | ~2.1 ms | ~1.65 ms | vs 2.9 ms deadline |

(The 0.999 ms baseline is the surgery-day A/B on one machine state; the
verification run a few hours earlier measured 1.052 ms median on the same
model — normal run-to-run machine variance, which is why the A/B was measured
as a pair.)

The −20.5% lands, as predicted, just above the input-scaled lower bound
(19.2%) — early conv layers still process the full 1655 window; only the
deeper, shrinking feature maps see the output-side saving. Combined with the
two other optimizations shipped the same day (AutoEQ display-curve decimation,
MelLimiter vDSP/sparse rewrite), non-subtimer stage means dropped
≈443 → ≈358 µs/block (~−19% of total; `perf_stage_ranking.md` §6). Files
changed for the bus comp itself: `model.onnx` (same 69,129 bytes — the surgery
changes constants, not size), `plugin_meta.json` (`trace_len: 1655`), the yaml
comment, and the new verify script. Zero C++.

And the safety net for the new zero-margin world: `test_ssl_hop_contract.cpp`
(§4.2) now stands between any future re-export and a DAW-side activate
failure.

---

## 9. Bundle wiring and failure modes

At module load (`entry_init`, `axon_plugin.cpp:2746+`):

1. `find_bundle_contents_()` locates the `.clap/Contents` dir;
   `Resources/` holds the bundles. `axon_meta.json` (schema v2, validated in
   `composite_meta.cpp`) maps roles → sub-bundle dirs; `"ssl_comp"` is one
   (`sub_bundles`, alongside `saturator` and the five `auto_eq_*` classes).
2. `ssl_comp` is **optional**: its meta is loaded only if the composite
   declares it (`:2829-2836`), setting `ssl_comp_loaded`. A missing bundle is
   a clean, silent passthrough — null session pointer, stage early-out
   (`:1842`), and its 896 samples excluded from reported latency (`:972`). By
   contrast, a missing saturator or Auto-EQ class throws inside `entry_init`,
   which catches and fails the whole plugin load.
3. `load_meta` (`meta.cpp`) parses `plugin_meta.json`, accepts schema 1 or 2,
   and cross-checks `stage_kind` against what is actually populated
   (`nn` must have `input_names` and no `dsp_blocks`, etc. — `:130-155`), so a
   miscomposed bundle fails loudly at load, not weirdly at runtime.
4. The per-channel `OrtMiniSession`s and all rings are built **at activate**
   (`:1191-1208`), once the host sample rate and channel count are known —
   and the host rate must equal the bundle's 44100 or activation fails
   (rate-specific model; no resampling — the standing v1 limitation).

`axon/export/composite.py` re-stages the sub-bundle into the composite on
every build (it validates `plugin_meta.json` but does not regenerate it) — the
meta and the model travel together, which is exactly what the resize relied
on.

Neighbouring stages for contrast, since all three "learned" stages run
differently: the **Saturator** (StageID 2) ships as an offline-distilled
rational polynomial — `stage_kind: "dsp"`, zero ORT at audio time (the
Rational-A math lives in [the Channel EQ dive](ssl-channel-eq.md)); it is
currently dormant (not in `processor_order`, zero calls). The **AutoEQ**
controller (StageID 1) is the third pattern
(`stage_kind: nn+dsp`): a stateful LSTM in ORT emitting band gains for a
native EQ renderer, one batch-2 call per 128-sample block — ungated, which is
why it is the chain's *second* CPU story (see [the Auto EQ dive](auto-eq.md)
and `native/clap/docs/perf_stage_ranking.md`).

---

## 10. Limitations, trade-offs, open ideas

**Inherent to the design:**

- **Fixed curve, fixed rate.** One compressor, one setting, 44.1 kHz only.
  The only "parameters" are the wet mix and the SSC_IN operating-point drive;
  fast SSC_IN automation causes a brief level bump (gain and reciprocal act
  896 samples apart — §5, documented in-source).
- **896 samples of conditional latency** and a ~23 ms dry warm-up after
  activate. Both are the direct price of hop batching; both are handled
  (PDC + pass-through) but real.
- **Zero contract margin.** `trace_len − rf − kSslHop == 0` by construction
  since the resize. Deliberate (that *is* the optimization), but it means any
  model or constant change trips the guard — which is the contract test's
  whole job. The measured effective RF (629 < 631) even hides two samples of
  unofficial slack; the declared value is the one enforced.
- **Host `reset()` does not clear the bus-comp state.** `plugin_reset`
  (`axon_plugin.cpp:1283-1318`) zeroes the block accumulators, the AutoEQ LSTM
  state and the module DSP, but not `ssl_comp_in_ring`/`in_accum`/`out_queue`
  — those re-initialize only on (de)activate. After a transport reset, up to
  ~1.6k samples of pre-reset context briefly colour the first hops (and up to
  one hop of stale wet can drain). Inaudible in practice for a bus comp with
  14 ms of memory, but it is asymmetry worth knowing about.
- **The hop spike is the cost profile.** Mean CPU is modest; the pair of
  forwards every 8th block is the p95. This is a scheduling artifact, not
  throughput — which is why the remaining ideas are all about *when*, not
  *how much*:

**Open, with numbers attached:**

- **L/R hop-phase stagger** (`docs/future/not-started/ssl-comp-hop-stagger.md`,
  issue #22) — offset channel 1's accumulator phase by `kSslHop/2` so each hop
  block runs *one* ~0.82 ms forward instead of two: block p95 est.
  2256 → ~1300 µs, zero throughput or latency change (the wet always trails by
  896 samples regardless of hop phase, and §8.2's causality proof is what
  makes the phase-independence argument rigorous). Byte-identity is *not*
  promised — same math at different tensor positions can differ in last-ULP
  SIMD tails — so acceptance is the steady-state null bar.
- **Worker-thread forward** (`perf_stage_ranking.md` #6) — move the forward
  off the audio thread at the cost of +1024 samples (+~23 ms) of reported
  latency; the RT thread sheds ~58% and the spike disappears (machine-total
  CPU unchanged). The cheaper +1-block variant is *not* safe unstaggered
  (2 × p95 1.9 ms > the 2.9 ms block period). Owner/product decision — latency
  is a user-facing promise.
- **ORT intra-op threads 2–4** (#7) — measured 1081 → 603/409 µs, standing
  *no* for DAW CPU-citizenship reasons (§6).
- **IoBinding / allocation-free `run()`** (#12, issue #18) — RT-safety
  hygiene, ~0.2% throughput; dovetails with the buffer-alignment hypothesis in
  the ORT nondeterminism investigation (issue #19).

**Superseded record:** this dive replaces `docs/neural_inference_ort.md` (written
against the 2048-era bundle: its `trace_len`/`actual_olen`/latency tables,
Saturator-in-chain framing and line anchors have all drifted; everything still
true from it is carried above). The full resize design record + re-export
playbook lived in `native/clap/docs/ssl_comp_reexport_handoff.md`, retired
after consumption per the docs lifecycle — recover via
`git show 634f980:native/clap/docs/ssl_comp_reexport_handoff.md`. The measured
current state of the whole chain remains `native/clap/docs/perf_stage_ranking.md`.
