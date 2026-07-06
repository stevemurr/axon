# The Widener: a Blumlein shuffler where mono-compatibility is algebra, not aspiration

Every stereo widener ships with the same disclaimer: "check your mix in mono." Haas-delay
wideners comb-filter when summed. All-pass decorrelators smear transients and collapse
unpredictably. Even "M/S" wideners that EQ the side and the mid separately can shift the mono
balance. The industry answer to mono compatibility is a correlation meter and a prayer.

Axon's Widener (`native/clap/src/widener.hpp`, 122 lines, header-only) takes a different
position: **mono compatibility is not a property you test for, it is a property you construct.**
The module is a frequency-dependent Mid/Side shuffler in the Blumlein/Gerzon tradition — it
decodes to M/S, applies a frequency-dependent gain *to the side only*, and re-encodes. One line
of algebra then guarantees that no combination of its knobs, at any frequency, at any setting,
can ever change what a mono listener hears. A second line guarantees that at the neutral setting
the module is not "transparent-ish" but **bit-identical** — the output floats equal the input
floats, exactly.

The same file also carries a small war story: its `reset()` contains a deliberately odd
`low_hz_ = -1.f` sentinel, built the same day a sneaky init-order bug was found in the (since
deleted) Exciter — a bug class where a freshly-instantiated plugin passes audio through
*unfiltered* until the user happens to touch a knob. The Widener was designed so that bug is
structurally impossible, and it has a regression test to keep it that way.

---

## 1. Signal flow

```
              ┌───────────────────────── M = 0.5(L+R) ──────────────────────────┐   L = M + S′
 L ──┐        │                                                                 ▼
     ├─ M/S ──┤                          ┌── LR4 HPF @ low_hz ──► S_hi ─ ×width ─┐
 R ──┘        │                          │                                       │
              └─ S = 0.5(L−R) ───────────┼──► S_lo = S − S_hi ──────────────────(+)──► S′
                                         │                                       │
                                         └── LR4 HPF @ 6 kHz  ──► S_air ─ ×air ──┘
                                                                                     R = M − S′
```

Per sample (`widener.hpp:79-95`, all math in `double`):

```
M     = 0.5·(L + R)                       // mid — NEVER touched
S     = 0.5·(L − R)                       // side
S_hi  = LR4_HPF(S, low_hz)                // side above the width crossover
S_lo  = S − S_hi                          // exact complement (see §3)
S_air = LR4_HPF(S, 6 kHz)                 // side above the fixed Air crossover
S′    = S_lo + width·S_hi + air·S_air
L     = M + S′,   R = M − S′
```

Three parameters: `width` (side gain above `low_hz`; 1 = neutral, 0 = mono above the crossover,
2 = double side), `low_hz` (the width crossover, 50–1000 Hz from the UI), and `air` (extra
high-frequency side above a fixed `kAirHz = 6000.0`, `widener.hpp:46`). There is **one** set of
side biquads — the filters run on the single side signal, not per channel.

In the default chain the stage is `StageID::Widener = 9` (`axon_plugin.cpp:116`), slotted
between the Reverb and the SSL bus comp: `kDefaultStageOrder{6, 3, 1, 8, 9, 4, 5}` =
BassMono → SslEq → AutoEQ → Reverb → **Widener** → SslComp → MelLimiter
(`axon_plugin.cpp:125`, ordering rationale comment at `:710-715`). The placement follows the
same logic the in-code comment gives for the reverb: anything that can raise peaks (and width
> 1 raises L/R peaks on side-heavy material) should sit *before* the bus comp and limiter so
they catch it. It is a single stereo instance on the `Plugin` struct, not per-`ChannelChain`
(`axon_plugin.cpp:698-700`) — M/S processing is inherently a two-channel operation. Note it is
also the exact functional inverse of the BassMono stage (`bass_mono.hpp`): BassMono removes side
*below* a cutoff; the Widener at `width = 0` removes side *above* one. Same LR4-on-the-side
machinery, opposite ends of the spectrum.

Latency: **zero**. Every path is minimum-phase IIR with no lookahead and no compensating delay,
so `compute_latency_` (`axon_plugin.cpp:941`) does not mention the stage at all — it
contributes nothing to the plugin's PDC.

## 2. The mono proof (why no knob can ever hurt you)

Re-encode the output and sum it to mono:

```
L′ + R′ = (M + S′) + (M − S′) = 2M = L + R
```

`S′` cancels *symbolically*. It does not matter what `S′` is — widened, mono'd, air-boosted,
phase-rotated by the IIR filters, or replaced by white noise. The mono sum is `2M`, and `M` is
never touched. This is the entire proof, and it is why the header comment calls the module
mono-compatible **by construction** (`widener.hpp:6-11`): the guarantee does not depend on
filter quality, coefficient precision, sample rate, or parameter values. There is no setting of
`width`, `low_hz`, or `air` that a mono broadcast chain can punish.

In exact arithmetic the invariance is exact. In the implementation, `M` and `S′` are computed
in `double` and the two outputs are each rounded to `float` once, so the *measured* mono-sum
error is one float-ULP-scale residual: the sweep test (`test_widener.cpp:69-96`) runs all 60
combinations of `width ∈ {0, 0.5, 1, 1.5, 2} × low_hz ∈ {50, 250, 600, 1000} × air ∈ {0, 0.5, 1}`
over a full second of independent-content stereo and measures

```
worst max|(L+R)_out − (L+R)_in| = 1.19e-07     (assert bar: < 1e-4)
```

i.e. ~−138 dBFS of rounding, not a property that degrades with settings.

Contrast this with what the proof does *not* promise: stereo playback. Width changes are fully
audible in stereo (that's the point), and `L − R = 2S′` changes freely. The guarantee is
precisely scoped: *the mono fold-down is invariant*.

## 3. The identity proof (why width = 1 is bit-exact, not "close enough")

The second constructive trick is the complement. `S_lo` is not produced by a matching low-pass
filter — it is produced by **subtraction**:

```
S_lo = S − S_hi          (widener.hpp:89)
```

so the split obeys `S_lo + S_hi = S` *exactly*, by definition, at every sample, regardless of
what the high-pass actually did. Substitute into the shuffle at `width = 1, air = 0`:

```
S′ = S_lo + 1·S_hi + 0·S_air = (S − S_hi) + S_hi = S
```

This matters because the obvious alternative — a designed LR4 *low-pass* running next to the
LR4 high-pass, classic crossover style — does **not** reconstruct: Linkwitz–Riley pairs sum to
an all-pass (flat magnitude, rotated phase), so "neutral" would still phase-shift the side
relative to the mid and `width = 1` would not be a null. The subtractive complement makes
perfect reconstruction an identity instead of a filter-design goal.

Two footnotes keep this honest:

1. **In floating point, `(S − S_hi) + S_hi` is *not* guaranteed to round back to `S`** — FP
   addition is not associative. So the implementation does not rely on the algebra for the
   bit-exactness claim; it has an explicit early-out (`widener.hpp:82`):

   ```cpp
   if (width_ == 1.f && air_ == 0.f) return;   // exact passthrough, filters skipped
   ```

   At neutral, `process()` returns before touching a single sample. Identity is enforced by
   control flow; the algebra explains why neutral is also *conceptually* neutral (the audible
   change as `width` moves off 1.0 is continuous from zero, no bypass "jump"). Test 1
   (`test_widener.cpp:45-64`) asserts `max|out − in| == 0.0` — the literal `==`, not a
   tolerance. The plugin dispatch has the same gate one level up (`axon_plugin.cpp:2084-2085`):
   `WID_ON` off, mono bus (`n_ch < 2`), or `amt == 1 && air == 0` all `break` before
   `set_params`/`process` are even called.

2. **The complement's magnitude response is not an LR4 low-pass.** With
   `H(f)` the LR4 high-pass, the low branch is `1 − H(f)`, whose upper skirt decays at only
   ~6 dB/oct (for LR4, `1 − h² = (1−h)(1+h)` and `1−h` is first-order at HF). The clean way to
   read the design is the total effective side transfer:

   ```
   G(f) = (1 − H(f)) + width·H(f) + air·A(f)
        = 1 + (width − 1)·H(f) + air·A(f)
   ```

   a smooth complex-plane interpolation from exactly 1 well below `low_hz` to `width` well
   above it (plus the additive air term above 6 kHz). "The lows stay neutral" is a soft
   crossover statement, not a brick wall — see §8.

## 4. The filters

Both side high-passes are **LR4**: two cascaded identical 2nd-order RBJ-cookbook Butterworth
sections at `Q = 1/√2`, giving 24 dB/oct and the Linkwitz–Riley property of −6 dB at the
crossover per branch. The coefficient recipe is the shared
`rbj_butterworth_hpf(fc, sr, out)` in `native/clap/src/biquad.hpp:59-73`, and the section is the
shared `BiquadTDF2` (`biquad.hpp:19-35`) — transposed direct form II, all-`double` coefficients
*and* state. That header exists because this exact struct had been copy-pasted into
bass_mono/widener/reverb/iir_filterbank_eq/ssl_channel_eq; it was extracted verbatim (commit
`c208fdd`) with token-identical `process()` bodies so renders stayed byte-identical.

Design happens in `design_hpf_` (`widener.hpp:105-109`), which also carries the safety clamp:
a requested cutoff outside `(1 Hz, 0.49·sr)` falls back to a per-band default (250 Hz for the
width band, 6 kHz for air). This is the call-site clamp policy the biquad header explicitly
leaves to callers. Four biquads total: `hp_lo1_/hp_lo2_` at `low_hz`, `hp_air1_/hp_air2_` at
6 kHz (`widener.hpp:118-119`).

Per sample the active path costs: one M/S encode, four double-precision biquads (~5 FLOPs
each), the three-term shuffle, one M/S decode — roughly 35 FLOPs. That's the whole module,
which is why it benchmarks in single-digit nanoseconds (§7).

Precision discipline matches the rest of the chain's IIR stages: `float` I/O, `double`
internally, `double` filter state. No denormal guards — the June 2026 DSP pass established
empirically (on the BassMono twin) that Apple Silicon flushes subnormals at full speed, so FTZ
guards are dead code on the shipping target.

"Phase-coherent" in the header (`widener.hpp:11`) means specifically: the width change is a
*gain* on minimum-phase-filtered components of the side — there is no inter-channel delay, no
all-pass decorrelation, no Haas trick. L and R stay time-aligned; near the crossover the side
picks up the IIR phase rotation of the blend (unavoidable for any zero-latency IIR split), but
mid/side alignment is never traded for width.

## 5. Plugin integration: the four controls and where each line lives

The control surface is `WID_ON` / `WID_AMT` / `WID_FREQ` / `WID_AIR`. Per the meta↔C++
contract, the schema source is `axon/export/composite.py:168-174` (which generates
`weights/axon_bundle/axon_meta.json`), and the read-set lives in `resolve_amount_`:

| Control | Name | Range | Shipped default | Read at | Snapshot field |
|---|---|---|---|---|---|
| `WID_ON` | Width | switch | **1.0 (ON)** | `axon_plugin.cpp:1422` | `wid_on` (`:1466`) |
| `WID_AMT` | Amount | 0..2 | **1.38** | `:1423` | `wid_amt` (`:1467`) |
| `WID_FREQ` | Low | 50..1000 Hz | 250 | `:1423` | `wid_freq` (`:1468`) |
| `WID_AIR` | Air | 0..1 | **1.0** | `:1424` | `wid_air` (`:1469`) |

The `AmountSnapshot` fields are declared at `axon_plugin.cpp:1346-1349`; pre-parse fallbacks
(`won=0, wa=1, wf=250, war=0` — i.e. absent controls mean a safe no-op) at `:1378-1379`. The
dispatch case is `axon_plugin.cpp:2078-2090`: gate on `wid_on`/`n_ch`, gate on neutral, then
`set_params` + in-place `process` on the 128-sample work block. `set_params`
(`widener.hpp:60-64`) is called every active block but is nearly free: width/air are clamped
scalars (DSP clamp is 0..4, wider than the UI's 0..2/0..1), and the filter design only reruns
when `low_hz` actually changes, thanks to the cached-cutoff comparison.

Lifecycle: `prepare(sample_rate)` in `activate` (`axon_plugin.cpp:1235`), `reset()` in
`plugin_reset` (`:1288`). UI: `PROCESSORS` entry id 9 with `wetParams: ['WID_ON']`
(`ui/index.html:504`), stage colour lavender `#c792ea` at `STAGE_COLORS[9]`
(`ui/index.html:1251`). Tests are wired into CTest (`CMakeLists.txt:166-167`).

**The defaults are a real editorial decision, and the code comments have drifted behind it.**
The chain *ships actively widened*: `WID_ON = 1`, Amount 1.38, Air 1.0 (in both
`composite.py` and the generated `axon_meta.json`). But the fallback control-injection block in
the plugin — used for bundles that don't declare the controls — injects those same values under
a comment that still reads "Defaults make the stage a no-op (WID_ON = OFF; …)"
(`axon_plugin.cpp:2791-2797`). The values are the truth; the comment describes the stage's
original 2026-06-06 defaults (switch OFF, amount 1.0, air 0.0), which were later retuned. When
reading that block, trust the `ControlSpec` literals, not the prose. (Same story in older
session notes that record "def OFF" — superseded by the shipped meta.)

## 6. The war story: `reset()` and the exciter-class init bug

The strangest-looking lines in the file are in `reset()` (`widener.hpp:66-76`):

```cpp
void reset() {
    hp_lo1_.clear(); hp_lo2_.clear();
    hp_air1_.clear(); hp_air2_.clear();
    low_hz_ = -1.f;      // stale sentinel → the NEXT set_params() re-designs
    design_air_();       // Air cutoff is fixed; keep its coeffs valid after reset.
}
```

Why would a reset *invalidate* a parameter cache and *re-design* a filter? Because of what
happened to the Exciter on the very day the Widener was written (2026-06-06, commit
`4367cc4`, "exciter init fix"). The failure chain there:

1. `prepare()`/activate designs the band-split filter coefficients. Good.
2. CLAP hosts call `reset()` **after** activate. The Exciter's reset re-initialized its filter
   objects — wiping the designed coefficients back to the default-constructed state. A
   default `BiquadTDF2` is `b0 = 1, everything else 0` (`biquad.hpp:20`): a **unity
   passthrough**, not a filter.
3. The first `set_params()` then compared the requested cutoffs against its cached "already
   designed at X" values. The defaults matched the cache, so it **skipped the re-design**.
4. Net effect: the band-split filters silently ran as passthroughs, and the exciter distorted
   the *full-range* signal instead of its band — until the user wiggled any knob, which forced
   a re-design and "fixed" it. A bug that vanishes the moment you poke at it is a special kind
   of miserable to diagnose. (The related `c91ac88` "reset() must not wipe band coefficients"
   in the Harmonics rework is the same class from the other direction.)

The general shape of the bug class: **a module that caches "designed at X" separately from the
filter objects can end up with the cache claiming validity the filters don't have.** Two
mutable copies of one fact, and `reset()` updated only one of them.

The Widener's defense is structural, and two-layered:

- `reset()` clears **only** the z-state (`clear()` zeroes `z1/z2`, never the coefficients), so
  a reset can't strand the filters at passthrough in the first place; *and*
- it stamps the cutoff cache with the `-1` sentinel anyway, so even if the coefficients ever
  were default (a future refactor, a reset-before-prepare host), the very next `set_params()`
  is guaranteed to re-design regardless of whether the requested value "matches". The cache is
  never allowed to claim validity across a reset. The Air filter has a fixed cutoff and no
  cache to invalidate, so it is simply re-designed unconditionally on the spot.

`prepare()` (`widener.hpp:48-55`) then calls `reset()` *followed by* `design_()`, so the first
`process()` after activation is band-limited even if the host never sends a parameter flush.
One nit for the careful reader: the trailing comment in `prepare()` claims a first
`set_params()` with the same `low_hz` "won't redundantly re-design" — but `design_low_()` never
writes `low_hz_` back, so after `prepare()` the cache still holds the sentinel and the first
`set_params()` always re-designs once. That's the safe direction of wrong (one redundant
coefficient computation, block-rate, ~a dozen transcendental ops), but the comment overpromises.

The regression test is Test 7, `test_init_does_not_strand_filters`
(`test_widener.cpp:207-241`), and it replays the exact CLAP sequence: `prepare()` → `reset()` →
`set_params(2.0, 250, 0)` where **250 is deliberately the design fallback default** — the value
most likely to hit a stale "already designed" cache. It then asserts both halves of the split:
5 kHz side energy must ~double (measured ratio 1.995) *and* 60 Hz side energy must stay put
(measured 1.003). The second assert is the clever one: if the filters were stranded at
passthrough, `S_hi == S`, so the width gain would apply *full-range* — the highs would still
double and a highs-only check would pass. Only the lows expose the strand.

## 7. Measured: cost and verification

**In-chain cost** (from `native/clap/docs/perf_stage_ranking.md`, the 2026-07-05
`AXON_STAGE_TIMING` instrumented ranking at buf=128, all-stages scenario):

| Metric | Value |
|---|---|
| Share of `process()` @128 | **0.143 %** (0.143 % @512 — buffer-size-invariant) |
| Mean per 128-sample block | 0.65 µs (~5.1 ns/sample) over 68,900 calls |
| p95 / max per block | 1.00 µs / 37.58 µs |
| Ranking verdict (§5, "explicitly-cheap stages") | *leave alone; no proposal clears any reasonable ROI bar* |

The June 2026 per-module DSP pass reached the same verdict earlier: "**Widener — no change.
5 ns/s, already clean.**" It is one of only two stages in the audio path (with AutoGain) that
have never needed a perf commit.

**Standalone micro-benchmark** (`native/clap/bench/bench_widener.cpp`, 25 lines, compiled
standalone with `c++ -O3 -std=c++17 -I src`; not in CMake). Re-measured 2026-07-06 from the
exact origin/main sources on Apple Silicon:

```
== Widener throughput (stereo, width 1.4 air 0.2) ==
  block=64   7.82 ns/sample   (0.04% of 128-blk budget)
  block=128  6.45 ns/sample   (0.03% of 128-blk budget)
  block=512  5.70 ns/sample   (0.03% of 128-blk budget)
  bypass (w=1,air=0): 0.13 ns/sample
```

The bypass line is the §3 early-out doing its job: neutral costs a branch, not four biquads.

**Unit tests** (`native/clap/tests/test_widener.cpp`, 7 tests, all asserting on measured
numbers; re-run 2026-07-06 from origin/main sources, all pass):

| # | Property | Measured |
|---|---|---|
| 1 | Identity: width=1, air=0 → bit-identical | `max|out−in| = 0.0` exactly |
| 2 | Mono-sum invariance across 60-combination param sweep | worst `1.19e-07` (bar 1e-4) |
| 3 | Width=2 doubles side above crossover, lows untouched | hi ratio 1.995, lo ratio 1.003 |
| 4 | Air boosts side >6 kHz, mids alone | 10 kHz ratio 1.342 (bar >1.25), 2 kHz 1.006 |
| 5 | 4-second stability: finite, bounded, no DC | peak 0.858, DC ~3e-10 |
| 6 | Mono input (`r == nullptr`) is a graceful no-op | `max|out−in| = 0.0` exactly |
| 7 | Init regression (exciter-class bug, §6) | hi 1.995 / lo 1.003 |

Note the Air expectation in Test 4: **1.34×, deliberately not 2×**. `S_air` is an IIR-filtered
(hence phase-rotated) copy of the side *added on top of* the side that's already there, so the
RMS grows but not coherently — the test comment documents this as "a clear, audible widening",
not a calibrated +6 dB.

**Conditioning.** The Widener belongs to the chain's *well-conditioned* club (with BassMono,
Reverb, TruePeakCeiling): a pure feed-forward IIR pipeline with no FFT, no min-phase
reconstruction, no adaptive smoothing loop to amplify 1-ULP differences. Refactors here can be
validated with the strict two-binary null test and the null comes back **exactly 0.0** — unlike
Auto-EQ and MelLimiter, where build-level FP noise blooms to −75..−90 dBFS and only
steady-state/algebraic validation is possible. If you touch this file, demand the exact null;
it is achievable and anything less is a real change.

## 8. Limitations and open edges

- **Mono-safe is not peak-safe.** Width > 1 raises L/R peaks on side-heavy material (test 3's
  hard-panned case literally doubles one channel's side contribution). The stage does nothing
  about it *by design* — in the default order SslComp and the MelLimiter/true-peak ceiling sit
  downstream and catch it. But stages are freely reorderable: drag the Widener after the
  limiter and its peaks reach the output uncapped. Nothing warns about that ordering.
- **Mono-safe is not correlation-safe.** At high width the L/R correlation above the crossover
  can approach zero or go negative; the mono *sum* is provably fine, but heavily-widened
  material can still sound phasey on narrow stereo playback. There's no correlation tap wired
  to this stage in the UI.
- **The crossover is soft.** The complement's upper skirt decays at ~6 dB/oct (§3), so
  "lows stay neutral" means "side gain glides from `width` down to 1 through the crossover
  region", not a 24 dB/oct fence. For a widener this is arguably a feature (no audible seam);
  it is not what someone reading "LR4" might assume about the *untouched* branch.
- **Air is a blend, not a dB knob.** The additive, phase-rotated air term (§7) means `air=1.0`
  ≈ +2.6 dB side RMS at 10 kHz, and the exact figure is content- and frequency-dependent. Fine
  for an ear-driven control; unusable as a calibrated shelf.
- **State freezes across the early-outs.** When gated (WID_ON off, or neutral, or the
  `n_ch < 2` gate), the side filters don't run, so their `z` state and any pending `WID_FREQ`
  change go stale until re-engage; re-engaging resumes from old filter memory. In practice the
  side filter state is tiny (4 doubles) and no artifact has ever been observed, but it is not
  a crossfaded bypass.
- **Coefficient swaps aren't smoothed.** A `WID_FREQ` move re-designs the LR4 pair at block
  rate (128 samples ≈ 2.9 ms) with no coefficient interpolation; a fast automated sweep could
  zipper. Width/air moves are pure gains at block rate — effectively benign.
- **Comment drift, twice.** The injection-fallback comment at `axon_plugin.cpp:2791-2793`
  still describes the original no-op defaults (WID_ON OFF) while injecting the shipped
  ON/1.38/1.0; and `prepare()`'s trailing comment overstates the design-cache behavior
  (§6). Neither affects behavior; both should lose to the code when they disagree.
- **Fixed 6 kHz air crossover.** `kAirHz` is a constant, not a control — a deliberate
  two-knobs-not-three simplification. If a variable air crossover is ever wanted, it must
  copy the `low_hz` cache-plus-sentinel pattern, not the unconditional-redesign pattern.

## 9. Source map

| What | Where (origin/main) |
|---|---|
| Module (algorithm + rationale header comment) | `native/clap/src/widener.hpp` (`:1-35` comment, `:79-95` process, `:66-76` reset) |
| Shared biquad + RBJ HPF design | `native/clap/src/biquad.hpp:19-35`, `:59-73` |
| StageID / default order / instance | `native/clap/src/axon_plugin.cpp:116`, `:125`, `:698-700` |
| Lifecycle (prepare / reset) | `axon_plugin.cpp:1235`, `:1288` |
| Controls: snapshot / parse / assign | `axon_plugin.cpp:1346-1349`, `:1422-1424`, `:1466-1469` |
| Dispatch case + gates | `axon_plugin.cpp:2078-2090` |
| Control injection fallback (stale comment) | `axon_plugin.cpp:2791-2797` |
| Schema source / shipped defaults | `axon/export/composite.py:168-174` → `weights/axon_bundle/axon_meta.json` |
| UI stage entry / colour | `native/clap/ui/index.html:504`, `:1251` |
| Tests (7, incl. init regression) | `native/clap/tests/test_widener.cpp` (CTest via `CMakeLists.txt:166-167`) |
| Micro-benchmark | `native/clap/bench/bench_widener.cpp` |
| In-chain cost + leave-alone verdict | `native/clap/docs/perf_stage_ranking.md` §2, §5 |
| Origin commits | `d1942d1` (stage added, 2026-06-06), `4367cc4` (exciter init fix, same day), `c208fdd` (biquad extraction) |
