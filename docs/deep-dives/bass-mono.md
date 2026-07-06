# Bass Mono: the 4.7-nanosecond stage that cannot break your mono sum

Wide bass ruins masters in three specific, physical ways: it makes vinyl
cutting styli jump out of the groove, it cancels itself on club systems that
sum the low end into a single mono subwoofer, and it smears the phase
coherence that makes a kick drum punch. The standard fix is "mono the bass" —
and most implementations of that fix come with a caveat: *we tested it, and
the mono sum seems fine.*

Axon's version comes with something stronger: a three-line algebraic proof
that the mono sum **cannot** change, no matter what the filter inside does —
even if its coefficients are garbage. The mid channel is simply never in the
filter's signal path, so `L+R` out equals `L+R` in as an identity, not a test
result. That property is why `BassMono` sits **first** in the default chain,
why it nulls to *exactly* `0.0` in two-binary regression tests while its
neighbors can't, and why the perf pass measured it at **4.7 ns per sample**
(~0.02% of the real-time budget) and concluded: leave it alone.

At 71 lines, `bass_mono.hpp` is the shortest DSP source in the chain, which
makes it the ideal on-ramp for two things this doc covers in full: **mid/side
thinking** (the decomposition every stereo tool in Axon is built on), and
**what it takes to bolt any new stage into the reorderable CLAP chain** —
because BassMono was the module used to establish that checklist.

Sources: `native/clap/src/bass_mono.hpp`, `native/clap/src/biquad.hpp`,
`native/clap/src/axon_plugin.cpp`, `native/clap/tests/test_bass_mono.cpp`,
`native/clap/bench/bench_bass_mono.cpp`. All line references are to
`origin/main`.

---

## 1. The problem: low-frequency width

A stereo signal is two channels, `L` and `R`. Where they agree, you hear a
centered image; where they differ, you hear **width**. Mid/side (M/S) is just
the change of basis that separates the two:

```
mid   M = ½(L + R)     what both speakers agree on  → centre image
side  S = ½(L − R)     the disagreement             → stereo width
```

It's exactly invertible — `L = M + S`, `R = M − S` (check: `M + S =
½(L+R) + ½(L−R) = L` ✓) — so any processor can decompose, treat M and S
differently, and recombine losslessly.

Low frequencies (kick, bass, sub synths) often carry side energy — sometimes
deliberately, often as an accident of sample libraries or stereo effects. Why
that's a problem:

- **Vinyl.** The groove encodes L and R as the two groove walls;
  out-of-phase low frequencies translate to *vertical* stylus motion, which
  limits how loud the record can be cut and can literally throw the needle.
  Mastering engineers mono the bass to keep records cuttable.
- **Clubs and PAs.** Big systems sum bass to a mono sub. Side-channel bass is,
  by definition, the part that cancels in a mono sum — your low end
  disappears on exactly the systems where it matters most.
- **Phase and punch.** Slightly out-of-phase low end makes the mono sum
  weaker and smearier. Phase-coherent bass sums efficiently, which also
  means the limiter downstream can push the track louder for the same peak —
  a free loudness win.

The crucial framing: this module removes low-frequency **width**, not
low-frequency *level*. The bass stays exactly as loud; it just all points the
same direction.

## 2. The whole algorithm

Decompose to M/S, high-pass **only the side**, recombine. That's the entire
stage — here is the complete hot loop (`bass_mono.hpp:45-53`):

```cpp
void process(float* l, float* r, int n) {
    for (int i = 0; i < n; ++i) {
        const double m = 0.5 * (static_cast<double>(l[i]) + r[i]);
        const double s = 0.5 * (static_cast<double>(l[i]) - r[i]);
        const double sh = hp2_.process(hp1_.process(s));   // side, HPF'd
        l[i] = static_cast<float>(m + sh);
        r[i] = static_cast<float>(m - sh);
    }
}
```

```
         ┌──────────────┐   ┌──────────────┐
  S ────▶│ Butterworth  │──▶│ Butterworth  │────▶ S_hp   (side: bass width gone)
         │ HPF hp1_     │   │ HPF hp2_     │
         └──────────────┘   └──────────────┘
              (12 dB/oct each = LR4, 24 dB/oct)
  M ────────────────────────────────────────────▶ M      (mid: untouched)

  L' = M + S_hp        R' = M − S_hp
```

- Below the cutoff, `S_hp → 0`, so `L' = R' = M`: **mono**.
- Above the cutoff, `S` passes intact: full width preserved.
- The mid never sees a filter. That single structural decision is where every
  guarantee in this doc comes from.

## 3. The math

### 3.1 The constructive mono-sum guarantee

Sum the two output equations:

```
L' + R' = (M + S_hp) + (M − S_hp) = 2M = L + R
```

`S_hp` cancels *symbolically* — the identity holds for **any** `S_hp`
whatsoever. A mistuned filter, a NaN-adjacent filter, a filter replaced by a
bitcrusher: the mono sum still comes out exactly `L+R`. "Mono compatibility"
here is not a property the tests demonstrate; it's a property of the block
diagram. The tests merely confirm the code implements the diagram.

The only place reality intrudes is float rounding, and even there the
structure is unusually kind:

- `l[i] + r[i]` computed in `double` is **exact** (a double holds the sum of
  two floats without rounding), and `× 0.5` is exact (power of two). So `m`
  is the *bit-exact* mid of the input.
- The only rounding in the whole path from input sum to output sum is the two
  final `double → float` casts. So `(L'+R') − (L+R)` is bounded by two output
  ulps — about 1e-7 relative.

The unit test asserts `max |(L+R)_out − (L+R)_in| < 1e-4`
(`test_bass_mono.cpp:82-101`) — conservative by roughly three orders of
magnitude.

Two practical corollaries:

1. **Mono playback is bit-transparent** (up to those two ulps). On a mono
   system the track sounds identical with the stage on or off — by design.
   If you A/B in mono to hear "how much bass it removed," you will hear
   nothing; listen in stereo or watch a correlation meter.
2. The mid's tonal balance can never shift. BassMono is incapable of being an
   EQ, even by accident.

The same construction powers the Widener (StageID 9) in reverse — it *gains*
the side above a crossover instead of removing it below one — which is why
both stages advertise "mono-compatible by construction."

### 3.2 The side filter: 4th-order Linkwitz-Riley high-pass

How cleanly "mono below, stereo above" splits is set by the filter's slope.
BassMono uses an **LR4** high-pass: two identical 2nd-order Butterworth
sections in cascade, 24 dB/octave total (`bass_mono.hpp:59-64`):

```cpp
void design_() {
    const double fc = (cutoff_ > 1.0 && cutoff_ < 0.49 * sr_) ? cutoff_ : 250.0;
    // LR4 = two identical Butterworth sections cascaded (24 dB/oct).
    rbj_butterworth_hpf(fc, sr_, hp1_);
    rbj_butterworth_hpf(fc, sr_, hp2_);
}
```

Loading *identical* coefficients into both biquads is precisely what makes
the cascade a Linkwitz-Riley alignment rather than two arbitrary filters.
Properties that matter here:

- **Slope.** 24 dB/oct means side energy one octave below the cutoff is down
  24 dB, two octaves down 48 dB — a tight transition with no low-frequency
  width creeping through just under the knee.
- **−6 dB at the cutoff.** Each Butterworth section is −3 dB at `fc`, so the
  cascade is −6 dB: at exactly the `BMF` frequency the width is *halved*, not
  gone. "Mono below the cutoff" is asymptotic (see §8).
- **Monotonic, well-behaved response.** Butterworth Q = 1/√2 is the maximally
  flat alignment — no resonant bump that would *exaggerate* width just above
  the cutoff. (The classic LR virtue — the split bands summing back to
  allpass — isn't structurally needed here because the low side is discarded
  rather than recombined; what's being bought is the steep, hump-free,
  predictable-phase slope. Contrast the Widener, which *does* reconstitute
  its low side as an exact complement `S_lo = S − S_hi` to be an identity at
  width = 1.)

### 3.3 The biquad and its coefficients

Since the July 2026 cleanup the 2nd-order section lives in the shared header
`biquad.hpp` — the exact struct that had been copy-pasted into bass_mono /
widener / reverb / iir_filterbank_eq / ssl_channel_eq, extracted verbatim so
codegen and FP results are unchanged and renders stay byte-identical
(`biquad.hpp:1-11`). BassMono aliases it as its `Biquad`
(`bass_mono.hpp:56`).

It's a **transposed direct form II** (TDF2) section in `double`
(`biquad.hpp:19-32`):

```
y[n] = b0·x[n] + z1
z1   = b1·x[n] − a1·y[n] + z2
z2   = b2·x[n] − a2·y[n]
```

`z1`/`z2` are the filter's two words of memory — this is what makes it IIR
and what makes its latency zero: output appears on the same sample as input.
TDF2 is the standard choice for float-fed double filters: state words carry
partial sums (good round-off behavior) and there are exactly two of them.

The coefficients come from the RBJ Audio EQ Cookbook high-pass with
Q = 1/√2, shared by every LR4-building module in the chain
(`biquad.hpp:59-73`):

```
w0 = 2π·fc/sr        cw = cos(w0)       sw = sin(w0)
α  = sw / (2Q),  Q = 0.70710678
a0 = 1 + α

b0 =  (1+cw)/2 / a0        a1 = −2·cw    / a0
b1 = −(1+cw)   / a0        a2 = (1−α)    / a0
b2 =  (1+cw)/2 / a0
```

Everything is pre-divided by `a0`, so the difference equation runs with an
implicit `a0 = 1` (`BiquadTDF2::set`, `biquad.hpp:29-31`). Note the header's
contract: `fc` must arrive already validated — **each caller keeps its own
clamp policy at the call site** because they intentionally differ.
BassMono's policy is the `design_()` guard above: a cutoff ≤ 1 Hz or ≥ 49%
of the sample rate (where the bilinear-transform design misbehaves near
Nyquist) silently falls back to 250 Hz rather than producing garbage
coefficients (`bass_mono.hpp:60`).

## 4. The class, method by method

File: `native/clap/src/bass_mono.hpp`, class `nablafx::BassMono`. Four public
methods, three data members, no allocations anywhere.

**`prepare(double sample_rate)`** (`bass_mono.hpp:28-32`) — stores the rate,
clears filter state, designs coefficients. The guard on line 29 is a real
bug fix with a paper trail:

```cpp
sr_ = sample_rate > 0.0 ? sample_rate : 44100.0;
```

The original code assigned `sr_` unguarded, so `prepare(0.0)` computed
`w0 = 2π·fc/0 = inf`, yielding NaN/inf coefficients that poisoned every
output sample. A negative rate was subtler — a finite but *wrong* (negative)
`w0`, i.e. a corrupt filter that still looked alive. Both paths are pinned by
regression tests that assert finite output **and** that the clamped fallback
still behaves like a real bass-mono, not a dead pass-through
(`test_bass_mono.cpp:128-172`).

**`set_cutoff(float hz)`** (`bass_mono.hpp:34-39`) — the change-detection
guard:

```cpp
if (hz == cutoff_) return;
cutoff_ = hz;
design_();
```

The plugin calls this every block with the current `BMF` value, so the guard
is what keeps `sin`/`cos` out of the steady-state hot path — coefficients are
recomputed only when the knob actually moves. Deliberately, it does **not**
clear filter state: a moving cutoff retunes the running filter smoothly
instead of clicking. (The exact float compare is correct here: the value
either changed or it didn't; there's no accumulation.)

**`reset()`** (`bass_mono.hpp:41`) — zeroes both biquads' `z1/z2` so a
transport relocation doesn't leak stale side-channel history into the new
position. Wired to CLAP's reset callback (`axon_plugin.cpp:1286`).

**`process(float*, float*, int)`** (`bass_mono.hpp:45-53`) — shown in §2.
In-place, stereo, promotes to `double` for the M/S math and the IIR
recursion, casts back to float on write. Per stereo sample: 2 adds + 2
multiplies for M/S, two biquads (5 mul + 4 add each) on the side, 2 adds to
recombine — arithmetic only, no branches, no memory beyond four state
doubles.

Defaults: `sr_ = 44100`, `cutoff_ = 250.f` (`bass_mono.hpp:66-67`). Note the
distinction that has already caused one doc to go stale: **250 Hz is the
class's internal default and clamp fallback; the shipped product default is
225 Hz**, set by the meta (§5.4). The class never decides what the user
hears — the plugin always writes `BMF` over it.

## 5. Anatomy of a stage: how BassMono bolts into the chain

BassMono was the module used to establish the add-a-stage checklist, so this
section doubles as the tour of every integration point a new stage must
touch. Stages are identified by a stable `StageID` and dispatched in a
user-reorderable `processor_order`.

### 5.1 Identity and ordering

`kNumStages = 7` (`axon_plugin.cpp:100`) and the enum
(`axon_plugin.cpp:106-117`):

```cpp
enum class StageID : int {
    AutoEQ = 1, Saturator = 2, SslEq = 3, SslComp = 4,
    MelLimiter = 5, BassMono = 6,
    // 7 retired (Exciter/Harmonics, removed 2026-07) — value gap kept so
    // processor_order and saved sessions never re-key the remaining stages.
    Reverb = 8, Widener = 9,
};
```

IDs are append-only and never recycled — slot 0 (ex-InputLeveler) and slot 7
(ex-Exciter) are gaps, kept so `processor_order` arrays in saved sessions and
the UI's stage-color table never re-key. The default order
(`axon_plugin.cpp:125`):

```cpp
constexpr std::array<int, kNumStages> kDefaultStageOrder{6, 3, 1, 8, 9, 4, 5};
```

That leading **6** is BassMono, first in the chain:
**BassMono → SslEq → AutoEQ → Reverb → Widener → SslComp → MelLimiter**. The
rationale is spelled out where `processor_order` is declared
(`axon_plugin.cpp:711-716`): the reverb sits *after* BassMono so its bass is
already tightened — the FDN never reverberates wide low end — and everything
downstream (compressor detector, limiter band solve) sees a phase-coherent
low end that sums efficiently. Mono-ing the bass first is also the cheapest
possible loudness assist for the limiter at the other end of the chain.

`kDefaultStageOrder` is the single source of truth three times over: it
initializes `processor_order` (`:716`) and `pending_order` (`:759`), and
`state_load` validates that any restored order is a **permutation of exactly
this stage set** (`axon_plugin.cpp:1024-1048`) — anything else (corrupt
session, hand-edited JSON, a future version's ids) would silently bypass
unknown stages via the dispatch switch's default and/or run one stage twice
against shared state. On mismatch it keeps the current order and says so on
stderr.

Reordering is drag-and-drop in the GUI: the UI sends the order to
`axon_on_order_change` (`axon_plugin.cpp:2293-2299`), which writes
`pending_order` under a mutex; the audio thread drains it with a
**`try_lock`** at the top of `plugin_process` (`:2314-2321`) so the swap is
atomic per block and the audio thread never blocks on the GUI.

### 5.2 Instance, lifecycle

Stereo modules whose channels interact live as **one** instance in the
`Plugin` struct, not per-`ChannelChain` — BassMono is definitionally
cross-channel (`axon_plugin.cpp:692-693`):

```cpp
// Bass mono-maker (stereo; collapses width below a cutoff).
nablafx::BassMono    bass_mono;
```

Lifecycle wiring: `prepare(sample_rate)` in activate
(`axon_plugin.cpp:1233`), `reset()` in the CLAP reset callback (`:1286`).
And that's all — because BassMono is a zero-latency IIR, it contributes
nothing to `compute_latency_` (`axon_plugin.cpp:941`), which never reads the
BM controls. (A lookahead or FFT stage would have to report its delay there;
this one has no bulk delay to report, only frequency-dependent phase in the
side channel near the cutoff.)

### 5.3 Parameters: from control id to audio thread

Per-block, `resolve_amount_` snapshots every control into a plain-value
`AmountSnapshot` so the stage loop reads a consistent set. BassMono's fields
are `bm_wet` / `bm_freq` (`axon_plugin.cpp:1341`), defaulted
`bmi=0, bmf=250` (`:1376`), read from the `"BMI"` / `"BMF"` controls
(`:1418`), and assigned (`:1459-1460`).

The read on line 1418 is subject to a HARD RULE documented right above
`resolve_amount_` (`axon_plugin.cpp:1365-1373`): every control read must use
the literal `c.id == "BMI"` spelling, because
`tests/test_control_contract.cpp` extracts the C++ read-set from this file
with exactly that regex and diffs it against the shipped `axon_meta.json`.
The contract "meta controls == resolve_amount_ read-set" is machine-checked;
a helper-function or lookup-table read would silently fall out of it. (This
contract exists because it *had* drifted once — the generator and the reader
disagreed until the test pinned them together.)

### 5.4 The meta, and why param ids never scramble

The schema source is `axon/export/composite.py` — the shipped
`weights/axon_bundle/axon_meta.json` is re-composed every build. BassMono's
controls (`composite.py:159-160`):

```python
_ctl("BMI", "Bass Mono",  0.0,   1.0,   1.0, "switch"),
_ctl("BMF", "Frequency", 20.0, 500.0, 225.0, "Hz"),
```

| Control | Name | Range | Shipped default | Meaning |
|---|---|---|---|---|
| `BMI` | Bass Mono | 0–1 (switch) | **1.0 — ON by default** | Wet/dry of the effect; UI shows a switch, internally a continuous blend (`bm_wet`) |
| `BMF` | Frequency | 20–500 Hz | **225 Hz** | Cutoff: width below is collapsed, above untouched (`bm_freq` → `set_cutoff`) |

Both defaults have moved since the original module doc: the stage now ships
**enabled** at **225 Hz** (the earlier release had `BMI` default 0.0 and
`BMF` 250 Hz). Typical mastering cutoffs run ~100–250 Hz; the exposed 20–500
Hz range keeps every reachable value far from the `design_()` clamp, so the
silent 250 Hz fallback is unreachable through the UI.

CLAP param ids are **FNV-1a hashes of `"<effect_name>:<control_id>"`**
(`param_id.cpp:5-23`) — a *stable* function of the string id, not an index
into the controls array. That is what lets controls be added, removed, or
reordered in the meta without ever scrambling existing DAW automation lanes:
`BMF`'s param id is the same in every build that has a `BMF`. The hash has no
collision handling, so `entry_init` fails loudly at load if two control ids
ever hash together (`axon_plugin.cpp:2917-2932`) rather than letting host
automation silently route to the wrong knob.

### 5.5 Dispatch: the stage case

All stages run in `flush_chain_block_` (`axon_plugin.cpp:1504`), which walks
`processor_order` over fixed `kBlockSize = 128`-sample work buffers
(`axon_limits.hpp:19`). BassMono's case (`axon_plugin.cpp:2052-2064`):

```cpp
case StageID::BassMono: {
    if (amt.bm_wet <= 0.f || n_ch < 2) break;
    plug.bass_mono.set_cutoff(amt.bm_freq);
    // Process a copy so bm_wet can dial the effect in/out.
    std::array<float,kBlockSize> bl{}, br{};
    std::copy_n(work_l, kBlockSize, bl.data());
    std::copy_n(work_r, kBlockSize, br.data());
    plug.bass_mono.process(bl.data(), br.data(), kBlockSize);
    blend_inplace_(work_l, bl.data(), amt.bm_wet, kBlockSize);
    blend_inplace_(work_r, br.data(), amt.bm_wet, kBlockSize);
    break;
}
```

What the plugin layer adds on top of the bare DSP:

- **Early-out** when `bm_wet ≤ 0` (bit-identical bypass — the filter isn't
  even ticked) or when the input isn't stereo. On a mono track there is no
  side channel; the correct answer is a clean no-op.
- **Wet/dry blend.** The DSP class is deliberately all-wet; partial strength
  lives entirely in the wrapper. The stage processes a stack **copy** and
  mixes it back with `blend_inplace_` (`axon_plugin.cpp:1496-1499`):

  ```cpp
  static void blend_inplace_(float* buf, const float* wet, float w, int n) {
      if (w>=1.f) { std::copy_n(wet,n,buf); return; }
      for (int i=0;i<n;++i) buf[i]+=(wet[i]-buf[i])*w;
  }
  ```

  At `w ≥ 1` it's a straight copy — no arithmetic touches the wet signal, so
  full-strength output is exactly the module's output. Worth knowing: a
  *partial* `BMI` blends M/S-processed audio with dry audio, which is still
  mono-sum-safe (both signals have the same mid, and blending is linear), but
  the effective side attenuation is `1 − w·(1 − |H(f)|)` — shallower than the
  LR4 curve, floored at `1−w`. The switch semantics in the UI (0 or 1)
  sidestep this; the continuous blend exists because every stage gets one for
  free.

- **One filter state, two consumers?** No — note the early-out means the
  biquad states only advance when the stage is actually audible, and since
  the module is a single instance fed only by this one case, there's no
  shared-state hazard to reorderings: wherever the user drags the stage, it's
  still the only writer of its own state.

### 5.6 UI

The stage appears in the GUI's stage list via one `PROCESSORS` entry
(`ui/index.html:502`):

```js
{ id: 6, name: 'BASS MONO', params: ['BMI', 'BMF'], wetParams: ['BMI'] },
```

plus the default `procOrder = [6,3,1,8,9,4,5]` (`:511`, mirrored in the
`axonInit` fallback at `:1197`), the stage color `#90be6d` at
`STAGE_COLORS[6]` (`:1248`), and `'BASS MONO'` at `STAGE_NAMES[6]` (`:1253`).
Both tables are **indexed by StageID** — the other reason ids are never
recycled.

### 5.7 The checklist, distilled

For the next stage (this is exactly the path Widener followed a day later):

1. `axon_plugin.cpp`: bump `kNumStages`; append to `StageID`; add the DSP
   instance to `Plugin` (single instance for stereo-coupled modules); wire
   `prepare` in activate + `reset` in plugin_reset; update
   `kDefaultStageOrder` (which propagates to `processor_order`,
   `pending_order`, and state_load validation); add `AmountSnapshot` fields;
   parse the control ids in `resolve_amount_` **with the literal spelling**;
   add the `case` in `flush_chain_block_` with an early-out and
   `blend_inplace_` if a wet mix is wanted. Lookahead stages must also feed
   `compute_latency_`.
2. `axon/export/composite.py`: add the `_ctl` entries (the meta is
   generated — never hand-edit `axon_meta.json`). Stable-hash param ids mean
   this never disturbs existing automation.
3. `ui/index.html`: `PROCESSORS` entry, both `procOrder` defaults,
   `STAGE_COLORS`/`STAGE_NAMES` at the StageID index.
4. `CMakeLists.txt`: header-only DSP needs no plugin source entry, but
   register the unit test (`test_bass_mono`: `CMakeLists.txt:157-158`).

## 6. Measured: two perf verdicts and one global discovery

### 6.1 Standalone: 4.7 ns/sample, and the denormal experiment

The June 2026 DSP perf pass benchmarked every module standalone
(`bench/bench_bass_mono.cpp`, compiled directly against the header). On
Apple Silicon at `-O3`, BassMono measured **4.7 ns per sample** — about
0.02% of the real-time block budget. Verdict: **no change**; there is no
optimization here whose review cost is worth 4.7 ns.

The interesting result came from the bench's second scenario. An IIR filter
fed silence decays its state exponentially toward zero, which means the state
variables spend a long tail in the **subnormal** range — the classic
denormal-stall trap where x86 cores drop to microcode and a "cheap" filter
suddenly costs 100×. The bench constructs exactly this: a 0.2 s hard-panned
50 Hz burst followed by seconds of silence, driving the side-channel biquad
states through the denormal range — then times it with hardware
flush-to-zero **off and on**, toggling the FPCR FZ bit directly
(`bench_bass_mono.cpp:17-25`, scenario `:66-82`):

```cpp
#if defined(__aarch64__)
static inline void set_ftz(bool on) {
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    if (on) fpcr |= (1ULL << 24); else fpcr &= ~(1ULL << 24); // FZ bit
    __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
}
#endif
```

Result: **FTZ off and FTZ on were identical.** Denormals are genuinely
produced (the math guarantees it) but Apple Silicon processes subnormals at
full speed. This was the measurement that established a global finding for
the whole codebase: **FTZ guards are no-ops on this hardware** — no
`DAZ/FTZ` scaffolding, DC-offset injection, or state-flushing epsilon hacks
are needed in any Axon module on the shipping platform. (The bench keeps the
scenario so the claim is re-checkable per-port — on an x86 build this is the
first thing to re-run.)

### 6.2 In-context: 0.149% of process time

The July 2026 instrumented per-stage ranking
(`native/clap/docs/perf_stage_ranking.md`, `AXON_STAGE_TIMING` probes,
all-stages scenario at buf=128) puts BassMono at **0.149%** of process time
(0.148% at buf=512 — shares are buffer-invariant): mean **0.67 µs** per
128-sample block, p95 1.00 µs, over 68,900 calls. For scale, the bus-comp
TCN forward is 58% and the Auto-EQ LSTM 33.5%; BassMono sits in the ranking's
"explicitly-cheap stages — leave alone" list. Two ONNX models are the whole
CPU story of this plugin; the classical DSP rounds to zero.

### 6.3 Well-conditioned: the counter-example

Axon validates refactors with two-binary null tests: render the same input
through the old and new build and demand the difference stays below
−120 dBFS. Two stages *cannot* pass that bar even for algebraically
identical changes — SpectralMaskEq's min-phase-plus-smoother pipeline
amplifies a 1-ULP perturbation to ~−77..−89 dBFS, and MelLimiter's
sort-plus-branch water-filling solve is alignment-sensitive to ~−79 dBFS in
its warmup (see the ill-conditioned-stages notes; those stages get
steady-state-only or algebraic validation).

BassMono is the counter-example that proves the method isn't broken: it
**nulls to exactly 0.0** across binaries. The reasons are worth spelling out
because they're the checklist for building refactor-friendly DSP:

- the signal path is a fixed sequence of smooth arithmetic — no branches on
  data, no `std::sort`, no threshold selection that can flip a code path on a
  1-ULP input change;
- no FFT, so no library whose rounding depends on buffer alignment;
- scalar `double` code whose evaluation order the optimizer preserves (and
  which the shared-`biquad.hpp` extraction deliberately kept token-identical
  so codegen didn't shift);
- no recursion *on gains* — the IIR state is linear in the signal, so small
  perturbations stay small instead of being re-amplified by a
  decision-making feedback loop.

This is also why the `biquad.hpp` consolidation could promise byte-identical
renders, and why BassMono changes are cheap to review: the strictest
verification tool in the repo actually works on it.

## 7. The test suite

`tests/test_bass_mono.cpp` — six tests, standalone binary, registered as
`test_bass_mono` (`CMakeLists.txt:157-158`). Each pins one clause of the
module's contract:

| Test | Line | Pins |
|---|---|---|
| `test_low_freq_collapses` | `:41` | Hard-panned 50 Hz: side RMS drops >20 dB (ratio < 0.1) after settling |
| `test_high_freq_preserved` | `:62` | Hard-panned 5 kHz: side RMS ratio > 0.9 — width intact above cutoff |
| `test_mono_sum_preserved` | `:82` | Independent 80 Hz L / 3 kHz R: max mono-sum error < 1e-4 (§3.1 says the true bound is ~2 output ulps) |
| `test_mono_input_unchanged` | `:103` | Identical channels in → identical samples out (S = 0 ⇒ nothing to filter) |
| `test_prepare_zero_sample_rate` | `:128` | Regression: `prepare(0)` used to make `w0 = inf` → NaN coeffs; now clamps to 44.1k **and still collapses width** (guards against a "fixed" filter that's dead) |
| `test_prepare_negative_sample_rate` | `:155` | Regression: negative rate gave a finite-but-wrong negative `w0`; same clamp, same behavioral assertion |

The settling skips (`kSR/10` ≈ 100 ms) matter: an LR4 at 250 Hz has a real
impulse-response tail, and asserting on the transient would measure the
filter's startup, not its steady-state contract. And note the shape of the
two regression tests — they don't just assert "no NaN," they assert the
recovered filter *behaves*, because the negative-rate bug produced perfectly
finite garbage.

## 8. Honest limitations

- **Not a linear-phase split.** The side channel picks up minimum-phase
  rotation around the cutoff (the mid is phase-clean — untouched). Inaudible
  in practice, and irrelevant to mono compatibility (§3.1 holds regardless),
  but it means side content near `BMF` is slightly time-smeared relative to
  the mid. A linear-phase side HPF would fix that at the cost of real
  latency and pre-ringing on bass transients — a bad trade for a mastering
  chain whose whole low-latency story depends on stages like this one being
  free.
- **"Mono below the cutoff" is asymptotic.** LR4 is −6 dB at `fc`: at
  exactly 225 Hz the width is halved, and full collapse only arrives
  ~1.5–2 octaves down. If you need *sub-only* mono with full width at 100 Hz,
  set `BMF` low rather than expecting a brick wall at the knee.
- **It removes width, not level — so mono A/B is silent by design.** The
  right monitoring tools are a stereo listen, a goniometer, or a correlation
  meter; Axon's own meters (LUFS/RMS/peak) won't show the effect either,
  since none of them measures the side channel.
- **Wide-by-design bass gets centred.** A stereo-effected 808 tail or a
  chorus on a bass synth *is* low-frequency side energy; the module cannot
  distinguish accidental width from intentional width. Lower `BMF` or switch
  the stage off for material whose low end is wide on purpose.
- **Cutoff changes are block-quantized.** `set_cutoff` runs once per
  128-sample block from the resolved `BMF`, so a fast automation sweep steps
  the coefficients ~344×/s at 44.1k. Because state is preserved across
  retunes there's no click, and any residual zipper lives only in the side
  channel — in practice inaudible, but this stage has no per-sample
  coefficient smoothing and doesn't pretend to.
- **The `design_()` fallback is silent.** An out-of-range cutoff snaps to
  250 Hz with no error. Unreachable through the exposed 20–500 Hz range;
  reachable if you ever drive the class directly with garbage — in which
  case you get a working 250 Hz mono-maker instead of a diagnostic.
- **The class is all-wet and stereo-only by contract.** Reuse it elsewhere
  and you own the wet/dry mix and the `n_ch >= 2` check yourself; both live
  in the plugin wrapper, not the module.
- **On by default is a product stance.** Shipping `BMI = 1.0` at 225 Hz means
  every default render has its bass mono'd below 225 Hz. That's the intended
  "mastering-grade defaults" posture, but it also means null-testing Axon
  against another chain requires switching the stage off — the default is
  not a pass-through.
- **Open ideas: deliberately none.** The July ranking files BassMono under
  "explicitly-cheap — leave alone"; there is no perf or quality work queued.
  The one dormant hook is the FTZ experiment in the bench, kept as the
  first thing to re-run when a non-Apple-Silicon port (the active
  windows-linux-builds effort) needs to re-validate the "denormals are free"
  assumption on x86, where it will very likely be false and an FTZ/DAZ
  guard in the host process becomes necessary.

---

*This deep dive supersedes `docs/bass_mono.md` (2026-06-05), which described
the same algorithm accurately but predates the shared `biquad.hpp`
extraction, the BMI=1.0/BMF=225 Hz shipped defaults (it documented
off-by-default at 250 Hz), the Exciter retirement (`kNumStages` 8→7), the
current default order with BassMono first (it placed the stage "late, just
before the limiter"), and all current `axon_plugin.cpp` line anchors.*
