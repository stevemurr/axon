# The Neural Inference Engine (ONNX Runtime / ORT)

> The ORT session layer is the part of Axon that loads learned audio models exported as ONNX graphs and runs them, sample-block by sample-block, on the audio thread — most importantly the SSL-style bus compressor (a streaming Temporal Convolutional Network) and the per-class AutoEQ controller (an LSTM that emits EQ band gains). The Saturator, despite being "learned," ships as a distilled rational-polynomial nonlinearity and runs as pure DSP (no ORT call at audio time).

---

## 1. What is this?

### Modeling an audio effect with a neural network — in plain English

Classic audio effects (a compressor, a tube saturator, an EQ) are built from hand-written equations: "if the signal gets loud, turn it down with this attack/release curve," "bend the waveform with this transfer function," and so on. That works, but real analog gear has quirks — nonlinear interactions, frequency-dependent behavior, "character" — that are painful to capture by hand.

A **neural network** flips the problem around. Instead of writing the equation, you *show the network examples*: thousands of pairs of (input audio, output audio) recorded from a real unit. The network learns a function that maps input to output and, if trained well, reproduces the gear's behavior — including the quirks — on audio it has never heard.

So when we say "learned saturation" or "learned compression," we mean: a network was trained offline to imitate a target effect, and Axon runs that trained function in real time.

### What is ONNX? What is ONNX Runtime (ORT)?

- **ONNX** (Open Neural Network Exchange) is a *file format* for a trained network. The training happens in PyTorch (Python); the finished model is "exported" to a `model.onnx` file that describes the graph of math operations and their learned weights. Think of it as a portable recipe.
- **ONNX Runtime (ORT)** is the *engine that executes that recipe*. Axon links against the ORT C++ API (`onnxruntime_cxx_api.h`) and, for each block of audio, hands ORT the input samples and gets back the processed samples. No Python at runtime; just a fast C++ inference loop.

In Axon, every ORT model lives in a *bundle* — a directory under the plugin's `Resources/` (mirrored in `weights/axon_bundle/`) containing `model.onnx` plus a `plugin_meta.json` that describes its shape (see §5).

### What is a TCN? Receptive field? Streaming inference?

A **TCN (Temporal Convolutional Network)** is a stack of 1-D convolutions over time. Each layer mixes a sample with its neighbors; stacking many layers (with *dilations* that skip samples) lets a deep network "see" a wide window of recent audio while staying causal (output at time *t* depends only on samples at or before *t* — essential for real-time audio).

The **receptive field (rf)** is exactly how many past samples the output at time *t* depends on. For Axon's SSL compressor, `rf = 631` samples. To compute *one* clean output sample, the network must be fed that sample plus the previous 630.

**Streaming inference** is the trick that makes a long-rf TCN usable in real time. Rather than re-running the whole network on the entire song, we keep a sliding **ring buffer** of the most recent `trace_len` samples (2048 for SSL comp). Each call feeds the whole ring; the network returns predictions only for the positions where it had full context — i.e. positions `[rf-1, trace_len-1]`. The newest samples are the valid output; the oldest `rf-1` are "warmup" that primes the convolutions.

```
ring buffer (trace_len = 2048 samples)
┌──────────────────────────┬───────────────────────────────────────┐
│   history / "warmup"      │      valid predictions                │
│   positions [0 .. rf-2]   │      positions [rf-1 .. trace_len-1]  │
│   (630 samples)           │      (2048 - 630 = 1418 samples)      │
└──────────────────────────┴───────────────────────────────────────┘
                             ^ first sample with a full receptive field
ORT output length = trace_len - (rf - 1) = 2048 - 630 = 1418
```

**Analogy:** reading a sentence with a 3-word memory. You can only "understand" a word once you've seen the two words before it; the first two words of the page are just context you needed to get going. The TCN is the same, with a 631-sample memory.

---

## 2. Why it matters in mastering

Mastering engineers reach for two iconic colors over and over:

- **Bus compression** — the glue of an "SSL-style" bus comp that makes a mix feel cohesive and punchy. Its character lives in *how* it reacts (program-dependent attack/release, subtle harmonic coloration), which is exactly the kind of thing a TCN captures better than a textbook compressor equation.
- **Saturation** — gentle harmonic distortion that adds warmth and perceived loudness, the "tube/tape" sheen.

Axon learns both from reference gear. The SSL comp runs as a live neural network; the saturator was distilled into a rational polynomial (cheaper, and indistinguishable for a static nonlinearity). The **AutoEQ** stage uses a tiny learned LSTM to *listen* to the program material and suggest a frequency-balance curve per instrument class — an automatic tonal-balance assistant.

The net effect: analog-flavored processing with consistent, automatable behavior, all running inside one CLAP plugin.

---

## 3. The DSP/ML behind it — the streaming TCN export contract

This is the heart of the module. The contract between the Python exporter and the C++ runtime is small but strict.

### Key quantities (real values from `weights/axon_bundle/ssl_comp/plugin_meta.json`)

| Symbol | Meaning | SSL comp value |
|---|---|---|
| `trace_len` (N) | fixed input length the ONNX was traced at | `2048` |
| `receptive_field` (rf) | past samples needed per output sample | `631` |
| `actual_olen` | ORT audio-output length | `N - (rf-1)` = `2048 - 630` = `1418` |
| `num_controls` | parametric knobs into the graph | `0` |
| `state_tensors` | recurrent state carried between calls | none (stateless TCN) |
| `causal` | output depends only on past | `true` |
| `sample_rate` | rate the model was trained at | `44100` |

The plugin **refuses to activate** if the host sample rate differs from the bundle's `sample_rate` (the model is rate-specific; see the v1 limitations note at `axon_plugin.cpp:17-21`).

### The length relation

The single most important equation:

```
actual_olen = trace_len - (receptive_field - 1)
```

The ONNX has **no internal pre-padding**: it trims the first `rf-1` samples instead of zero-padding them, so a length-N input yields a length-`N-(rf-1)` output. Output index `i` corresponds to ring position `rf-1 + i`. This is stated verbatim in the code:

> "Streaming-mode TCN export contract: the ONNX takes `trace_len` samples in (the entire ring, including the `rf-1` history prefix) and produces `trace_len - (rf-1)` samples out — the model's predictions for ring positions `[rf-1, trace_len-1]`. Output position i corresponds to ring position (rf-1 + i)." — `axon_plugin.cpp:1359-1363`

### Ring-buffer priming and hop accumulation

Re-running a 631-rf TCN every 128-sample host block would blow the audio thread's deadline. Axon instead **accumulates** input into a hop-sized buffer and runs ORT once per hop:

```
constexpr int kBlockSize = 128;   // host/processing block
constexpr int kSslHop    = 1024;  // ORT runs once per 1024 input samples
```
(`axon_plugin.cpp:80,93`)

Each time the accumulator fills `kSslHop` samples, the ring is shifted left by `kSslHop`, the new hop is appended at the tail, ORT runs once over the full ring, and the **trailing `kSslHop` predictions** are stashed in a playback queue to be drained over the next `kSslHop / kBlockSize = 8` host blocks.

```
on accumulator full (every kSslHop=1024 samples):
  memmove ring left by kSslHop      ──► drop oldest 1024, keep newest 1024
  copy accumulator into ring tail   ──► [ ...kept... | new 1024 ]
  ORT run(ring[2048]) -> obuf[1418]
  take LAST kSslHop (=1024) outputs ──► outq   (these are ring positions [N-1024, N-1])
```

**The critical safety constraint** (asserted at activate, `axon_plugin.cpp:1007-1013`):

```
kSslHop <= trace_len - rf      (1024 <= 2048 - 631 = 1417  ✓)
```

If a hop shift discarded more than `trace_len - rf` samples, the newest output sample would no longer have a full receptive field behind it in the ring, producing a hop-rate discontinuity (audible flutter at 44100/1024 ≈ 43 Hz). Larger `kSslHop` => fewer ORT calls (less CPU) but more latency; it must stay under that bound.

### Saturator vs SslComp — different beasts

| | **Saturator** (StageID 2) | **SslComp** (StageID 4) |
|---|---|---|
| Runs ORT at audio time? | **No** | **Yes** |
| Implementation | `RationalA` polynomial `P(x)/Q(x)` | streaming TCN via `OrtMiniSession` |
| `architecture` in meta | `"dsp"` | `"tcn"` |
| `stage_kind` | `Dsp` | `Nn` |
| Receptive field | 1 (memoryless) | 631 |
| State | none | none (stateless conv) |
| Bundle ships `model.onnx`? | No (only `plugin_meta.json`) | Yes |
| Added latency | 0 | `kSslHop - kBlockSize` = 896 samples |

The Saturator's "neural" content was **distilled offline** into rational-polynomial coefficients stored in the bundle's `dsp_blocks[0]`. At runtime it is a per-sample evaluation of `P(x)/Q(x)` — see §7. The AutoEQ stage is a *third* pattern (`stage_kind = NnDsp`): a learned LSTM **controller** that runs in ORT and emits EQ band gains, feeding a native `SpectralMaskEq` DSP block.

### Block-size / sample-rate handling

- All ORT sessions process fixed `kBlockSize = 128` chunks at the *plugin* level (the AutoEQ controller's `cond_block_size` and the SSL comp's hop arithmetic both assume it). Changing `kBlockSize` requires re-exporting the ONNX bundles (`axon_plugin.cpp:76-80`).
- Sample rate is fixed per bundle (44100). The host rate must match or activation fails.

---

## 4. How it works in the code

There are **two** ORT wrappers in the tree. Know which is which:

1. **`OrtSession`** (`ort_session.cpp` / `ort_session.hpp`) — a general-purpose, fully real-time-safe wrapper using `Ort::IoBinding`-style pre-allocated tensors and A/B double-buffered state. It is the reference/utility session class and documents the contract cleanly. *(The live Axon chain currently drives its models through `OrtMiniSession`, below.)*
2. **`OrtMiniSession`** (`axon_plugin.cpp:366-560`) — the lighter wrapper actually instantiated for `ssl_comp` and each AutoEQ class in the running plugin.

Both follow the same export contract; document and reason about them together.

### Session creation

Sessions are created single-threaded on purpose so ORT never spawns a thread pool that could stall the audio thread:

```cpp
session_opts_.SetIntraOpNumThreads(1);
session_opts_.SetInterOpNumThreads(1);
session_opts_.SetExecutionMode(ORT_SEQUENTIAL);
session_opts_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
session_opts_.EnableMemPattern();
session_opts_.EnableCpuMemArena();
```
(`ort_session.cpp:24-29`; `OrtMiniSession` mirrors this at `axon_plugin.cpp:376-379`.)

Input/output **names** are read once from the session and copied into owned `std::string`s, because ORT's `AllocatedStringPtr` frees on destruction and we need stable `const char*` for every `Run()` (`ort_session.cpp:33-48`; `axon_plugin.cpp:381-397`). `OrtMiniSession` also pre-builds the `"<state>_in"` / `"<state>_out"` name strings so `run()` never constructs a temporary whose `.c_str()` would dangle.

### Tensor binding & `run()`

`OrtSession::run(input_len)` builds the input tensor list in ONNX declaration order — audio first (`shape {1,1,input_len}`), then controls `{1,num_controls}` if any, then the current-bank state tensors — and pre-sizes the audio output to the contract length:

```cpp
const int audio_out_len = input_len - (meta_.receptive_field - 1);
```
(`ort_session.cpp:123`)

`OrtMiniSession::run(...)` is the one the SSL comp calls. It is *defensive* about the output length: it asks ORT for the real element count and clamps the copy, zero-filling any tail, so a length mismatch produces silence rather than reading past the tensor:

```cpp
const int64_t actual_len = out_info.GetElementCount();
const int64_t copy_len   = std::min<int64_t>(audio_out_len, actual_len);
std::copy_n(aud_out, copy_len, audio_out);
if (copy_len < audio_out_len) std::fill(audio_out + copy_len, audio_out + audio_out_len, 0.0f);
```
(`axon_plugin.cpp:463-475`)

### State double-buffering (for stateful models like the AutoEQ LSTM)

The SSL comp is stateless, but the same machinery serves the AutoEQ LSTM (`root_h`, `root_c`, shape `[2,1,64]`). To guarantee input and output state never alias inside one `Run()`:

- `OrtSession` keeps A/B banks and flips `state_in_is_a_` via `swap_state_buffers()` after each successful run (`ort_session.cpp:85-87,111-135`). The plugin calls swap *explicitly* so an aborted block leaves state untouched.
- `OrtMiniSession` keeps `in_states_` / `out_states_` maps and `swap_state()` swaps the buffers (`axon_plugin.cpp:490-494`). The AutoEQ call site does `sess->run_controller(...); sess->swap_state();` (`axon_plugin.cpp:1233-1234`).

### `StageID::Saturator` call site (`axon_plugin.cpp:1272-1349`)

No ORT involved. Each sample is run through the distilled polynomial with pre/post gain, threshold normalization and a DC-corrected bias:

```cpp
const float dc = chain.saturator.eval(amt.sat_bias);      // bias-induced DC, removed below
const float x_in = blk[i] * pre * invT + amt.sat_bias;
wet_a[i] = (chain.saturator.eval(x_in) - dc) * T * pst;    // saturate, restore AC, scale
```
An optional HPF→LPF band-split lets you saturate only a frequency band (bass-preserved / treble-limited), recombining the unsaturated remainder. `chain.saturator` is a `RationalA` reset from `g_state->sat_rational` (`axon_plugin.cpp:998-999`).

### `StageID::SslComp` call site (`axon_plugin.cpp:1351-1460`)

The streaming loop described in §3. Skipped early if the wet mix is zero **or the bundle wasn't shipped**:

```cpp
if (amt.ssl_comp_wet <= 0.f) break;
if (!plug.chains[0].ssl_comp_ort) break;            // bundle missing -> passthrough
const int N           = g_state->ssl_comp_meta.trace_len;     // 2048
const int rf          = g_state->ssl_comp_meta.receptive_field; // 631
const int actual_olen = N - (rf - 1);                          // 1418
```
The single ORT call is:
```cpp
plug.chains[ch].ssl_comp_ort->run(ring.data(), N, obuf.data(), actual_olen,
                                  nullptr, 0, "audio_out");
```
The newest `kSslHop` outputs (`obuf + actual_olen - kSslHop`, length 1024) become the playback queue (`axon_plugin.cpp:1406-1417`). A per-channel **dry-delay ring** of `kSslHop - kBlockSize` samples time-aligns the dry path so the wet/dry blend doesn't comb-filter (`axon_plugin.cpp:1421-1456`). During the initial warm-up (queue not yet full), the stage passes the dry signal through untouched.

### Model metadata (`meta.{hpp,cpp}` and `composite_meta.{hpp,cpp}`)

- **`PluginMeta`** (`meta.hpp:70-93`) describes one sub-bundle: `architecture`, `receptive_field`, `trace_len`, `num_controls`, `state_tensors`, `input_names`/`output_names`, `stage_kind`, and any `dsp_blocks`. `load_meta()` parses `plugin_meta.json`, accepts `schema_version` 1 or 2, and **cross-checks** that `stage_kind` agrees with what's populated (e.g. `nn` must have `input_names` and no `dsp_blocks`; `dsp` is the inverse) — `meta.cpp:126-152`. `trace_len` defaults to `0` for legacy bundles (`meta.cpp:85`).
- **`CompositeMeta`** (`composite_meta.hpp:51-71`) is the top-level `axon_meta.json`: it maps roles → sub-bundle directories (`sub_bundles`, e.g. `"saturator"`, `"ssl_comp"`), the multi-class `auto_eq` table (`class_order`, `classes`, `default_class`), the host knobs, and the AMT amount-mappings. `load_composite_meta()` requires `schema_version == 2` and validates the auto-EQ class table (`composite_meta.cpp:23-64`).

### Bundle discovery & loading (`entry_init`, `axon_plugin.cpp:1960-2024`)

On module load:
1. `find_bundle_contents_()` locates the `.clap/Contents` dir; `Resources/` holds the bundles.
2. `axon_meta.json` is parsed; then `saturator/plugin_meta.json` (mandatory).
3. `ssl_comp` is **optional** — loaded only if `axon_meta.sub_bundles.count("ssl_comp")`, setting `ssl_comp_loaded = true` (`axon_plugin.cpp:1974-1979`).
4. Every AutoEQ class meta is loaded in `class_order`; the saturator's rational coefficients are pulled from `sat_meta.dsp_blocks[0]` into `st->sat_rational` (`axon_plugin.cpp:2007`).
5. A single shared `Ort::Env` is created (`axon_plugin.cpp:2017`).

Actual `OrtMiniSession` objects are constructed later, **at activate**, one per channel (`axon_plugin.cpp:1014-1018` for SSL comp; `axon_plugin.cpp:986` for each AutoEQ class), once the host sample rate and channel count are known.

### Behavior when a bundle is missing

This is graceful by design:
- **No `ssl_comp` bundle** → `ssl_comp_loaded` stays false, `ch.ssl_comp_ort` is `nullptr`, the `SslComp` stage early-returns (`if (!plug.chains[0].ssl_comp_ort) break;`), and its latency contribution is excluded (`axon_plugin.cpp:855-856`). The stage becomes a transparent passthrough.
- The saturator and at least one AutoEQ class are **mandatory**; their absence throws inside `entry_init`, which catches and returns `false` so the plugin simply fails to load rather than crashing.

---

## 5. Latency & performance

### Where time goes

- **One ORT call per `kSslHop = 1024` input samples** for the SSL comp, *per channel*. Each call runs the full 2048-sample TCN forward pass. Amortized: 1 call / 8 host blocks (1024/128). This is the dominant ORT cost in the chain.
- **One ORT call per 128-sample block** for the AutoEQ controller (a small LSTM, `[2,1,64]` state).
- **Saturator: zero ORT** — just `eval()` of a degree-6 numerator / degree-5 denominator polynomial per sample (Horner's method, `rational_a.hpp:38-55`).

### Real-time-safety

- Sessions are single-threaded (`intra = inter = 1`) — no thread-pool handoff (`ort_session.cpp:24-26`).
- `OrtSession` pre-allocates all I/O and state buffers and wraps caller-owned memory in `Ort::Value` per run, so `run()` does not allocate (`ort_session.cpp:56-68,99-135`). It also runs a **warmup** pass with zeros at construction to prime ORT's kernel caches / mem-pattern / arena, then resets state (`ort_session.cpp:70-77`). `OrtMiniSession::run()` does construct small `std::vector`/`Ort::Value` temporaries per call, but no large audio allocations; the heavy ring buffers are pre-sized at activate (`axon_plugin.cpp:1019-1031`).
- The hop scheme exists precisely to keep the worst-case audio-thread deadline bounded: paying one big TCN pass every 1024 samples instead of every 128.

### What runs where

- **Audio thread:** the SSL-comp hop loop and ORT `run()`, the AutoEQ controller `run_controller()`, all DSP. `compute_latency_()` is also called here after param drain.
- **Main thread / load time:** bundle discovery, JSON parsing, `Ort::Session` construction (and the warmup pass) at activate.

### Latency budget (`compute_latency_`, `axon_plugin.cpp:828-859`)

```
kBlockSize                 = 128                 (input accumulator, always)
+ TruePeakCeiling lookahead                       (always, once activated)
+ SpectralMaskEq n_fft     = 4096   (AutoEQ, only when EQ wet > 0)
+ (kSslHop - kBlockSize)   = 896    (SSL comp, only when SSC > 0 and loaded)
+ MelLimiter::kLatency              (only when MLI wet > 0)
```
The SSL comp contributes its 896-sample delay **only when engaged**; reported latency updates with parameter state.

---

## 6. Parameters — how knobs reach the model

The SSL comp's ONNX is **non-parametric** (`num_controls = 0`): there is no "ratio/threshold knob" wired into the graph. Its only user control is the wet/dry mix, applied *outside* the model:

- `SSC` (host knob) → `s.ssl_comp_wet = ssc * amt_ssl_comp.wet_mix_max` (`axon_plugin.cpp:1159`). At `0`, the stage is skipped entirely.

The **Saturator** is parametric, but its parameters modulate the *polynomial application*, not the polynomial coefficients (those are fixed by the bundle):

| Knob ID | Meaning | Use in code |
|---|---|---|
| `SDR` | pre-gain (dB) → "drive" | `pre = 10^(sat_pre_db/20)` (`:1273`) |
| `SVO` | post-gain (dB) | `pst = 10^(sat_post_db/20)` (`:1274`) |
| `STH` | threshold (dB) → input scale | `T = 10^(sth/20)`, `invT = 1/T` (`:1151,1276`) |
| `SBS` | bias (DC offset into the nonlinearity) | `x_in = ... + sat_bias` (`:1330,1341`) |
| `SMX` | wet/dry mix | `blend_(..., sat_wet_mix, ...)` (`:1345`) |
| `SHF`/`SLF` | HPF/LPF band-split cutoffs | engage only outside 21 Hz / 19 kHz (`:1277-1278`) |

For a model that *did* take controls, the path is: the plugin writes the current knob values into `controls_buffer()` / passes a controls pointer, and `run()` binds a `{1, num_controls}` tensor named `controls` right after the audio input (`ort_session.cpp:104-109`, `axon_plugin.cpp:425-431`). The AutoEQ controller uses the related `run_controller()` path: it normalizes the block by a peak-hold envelope to match the training distribution, runs the LSTM, and reads back the `[1,15,T]` (or `[1,64,T]`) sigmoid params — taking only the first sample per channel since they're constant within a block (`axon_plugin.cpp:500-533,1219-1234`).

---

## 7. Gotchas / things to watch

- **Receptive-field warm-up.** After activate (or `reset`), the SSL ring is zero. No valid output exists until the accumulator first fills `kSslHop` samples and an ORT call runs; until then the stage passes dry audio through (`axon_plugin.cpp:1439-1457`). Expect a short warm-up before the comp "engages."
- **Latency is conditional.** Engaging SSC adds 896 samples *mid-session*; the host is told via `compute_latency_`. The dry path is internally delayed to match — do not assume a fixed plugin latency.
- **The hop constraint is load-bearing.** `kSslHop <= trace_len - rf` (1024 ≤ 1417) is asserted at activate. Re-exporting the TCN with a *larger* rf or *smaller* trace_len can violate it and throw. Conversely, raising `kSslHop` trades CPU for latency and can break the bound.
- **`actual_olen` mismatch.** The ONNX has no internal padding; output is `N-(rf-1)`, not `N`. `OrtMiniSession` clamps and zero-fills defensively (`axon_plugin.cpp:463-475`), but a mis-exported bundle that pads internally would shift the prediction-to-ring mapping and produce hop-rate flutter.
- **Missing bundles fail silently (SSL) or hard (saturator/AutoEQ).** A missing `ssl_comp` is a clean passthrough; a missing saturator or AutoEQ class throws in `entry_init` and the plugin won't load.
- **Sample-rate lock.** Models are 44.1 kHz only; the plugin refuses activation at other rates.
- **Determinism.** Single-threaded, sequential execution with fixed weights makes inference deterministic for a given input + state. Stateful models (AutoEQ LSTM) depend on prior state, so a `reset_state()` is issued on host `reset()` and on a CLS class change to avoid bleeding activations from another signal class (`axon_plugin.cpp:1204-1210`).
- **State aliasing.** Never let an input state tensor and its output share storage within one `Run()`. The A/B (or in/out map) double-buffering exists exactly to prevent this; `swap` is called *after* a successful run, explicitly, so aborts don't corrupt state.

---

## 8. Where it sits in the Axon chain

```
audio
  └─► ort(AutoEQ controller LSTM) ─► SpectralMaskEq      (StageID 1, NnDsp)
        └─► RationalA  (Saturator, distilled poly)        (StageID 2, Dsp)
              └─► ort(SSL bus comp TCN)                    (StageID 4, Nn)
                    └─► BassMono ─► MelLimiter ─► TruePeakCeiling ─► trim ─► output
```
(See the file header, `axon_plugin.cpp:1-9`.) Stages are user-reorderable; IDs are stable across past refactors (0 and 3 are intentionally retired).

### Related modules

- **Rational-A nonlinearity** (`rational_a.hpp`) — the distilled saturator math, `y = P(x)/Q(x)` with `Q` summing absolute terms for guaranteed positivity. Coefficients come from the `saturator` bundle's `dsp_blocks[0]` (degree-6 numerator, degree-5 denominator in the shipped bundle).
- **AutoEQ / SpectralMaskEq** (`spectral_mask_eq.{hpp,cpp}`, meta `SpectralMaskEqParams`) — the STFT magnitude-mask EQ driven by the per-class LSTM controller. Geometry from the bundle: `n_fft = 4096`, `hop = 2048`, `n_bands = 64`, `±18 dB`, `30 Hz–22.05 kHz`. One ORT session per class (bass/drums/vocals/other/full_mix), selected by the `CLS` knob; latency `n_fft - hop`.
- **`OrtSession`** (`ort_session.{hpp,cpp}`) — the canonical, allocation-free ORT wrapper and reference implementation of this same export contract, with explicit A/B state double-buffering and a constructor-time warmup.
- **`OrtMiniSession`** (`axon_plugin.cpp:366-560`) — the wrapper actually instantiated for the live SSL-comp and AutoEQ models.
- **Composite metadata** (`composite_meta.{hpp,cpp}`, `meta.{hpp,cpp}`) — the bundle/role wiring and per-model shape descriptors that make all of the above data-driven.
