# The Reverb: an 8-line FDN designed by subtraction — no bass, no colour, no latency, no loudness

Most reverbs are designed by addition: more early reflections, more modulation, more character. A mastering reverb has to be designed by *subtraction*, because on a master bus every classic reverb behaviour is a defect. Reverb tails dump energy under the mix (mud). Short delay loops ring at audible periods (flutter, metallic colour). Decorrelated stereo done lazily inverts one channel and vanishes on mono playback. Wet blends shift perceived loudness, which corrupts every A/B the user makes. And a pre-delayed dry path adds latency the host has to compensate.

Axon's reverb (`native/clap/src/reverb.hpp`, header-only, 315 lines, no CLAP/ORT deps) is an 8-line Feedback Delay Network whose every structural choice exists to *forbid* one of those failure modes. The header comment states the brief — "a little goes a long way" — and the whole file reads as a list of refusals:

| Classic failure | The thing that forbids it | Where |
|---|---|---|
| Bass mud | Send high-pass: **bass is never reverberated**, dry low end untouched | `reverb.hpp:126-127`, `:252-257` |
| Flutter / metallic ring | Mutually-prime delay lengths {443…1481} — echo periods never coincide | `:281-283` |
| Coloured, uneven tail | **Lossless** Householder feedback matrix — every mode decays at the same controlled rate | `:147-152` |
| Instability | Damping LPF with **exactly unity DC gain** + feedback capped at 0.9995 — loop gain provably < 1 | `:230-243` |
| Mono collapse | Two *different* ±1 tap rows for L/R (R is **not** −L), crossfaded against the plain mono sum | `:287-292`, `:176-186` |
| Loudness creep | Dry at unity, wet energy-normalised across Size/Damp: `wet_norm = 0.85/(1+0.6·size)` | `:188-190`, `:264` |
| Latency | Pre-delay is **wet-only**; dry is never delayed; `latency_samples()` returns 0 | `:131-137`, `:195` |
| DAW-breaking bypass | `mix == 0` early-returns — output is **bit-identical** to input | `:114` |
| RT hazards | All buffers max-sized in `prepare()`; zero allocation in `process()` | `:61-79` |

The rest of this document goes through each refusal down to the math, then the plugin integration, the measured numbers, and the honest trade-offs.

## Signal flow

From the header diagram (`reverb.hpp:6-9`):

```
input ──┬──────────────────────────────────────────────────► (+) ── out
        │  (dry: UNTOUCHED, unity, ZERO latency)                ▲
        └─► [send HPF] ─► [pre-delay] ─► 8-line FDN ─► width ─► mix ┘
             (low cut)      (wet only)    (Householder)   (decorr)
```

Per sample, `process()` (`reverb.hpp:113-192`) does, in order:

1. **Send** (`:119-128`): form a mid-dominant stereo pair — `sendL = HPF(mid + 0.5·side)`, `sendR = HPF(mid − 0.5·side)`, i.e. a 75/25 own/other blend — and high-pass both through a shared-header RBJ Butterworth (Q = 1/√2, `biquad.hpp:59-75`). The ±0.5 side term seeds a genuinely stereo field into the network; the HPF is the "no bass" refusal.
2. **Pre-delay, wet only** (`:130-137`): the send is written into a small stereo ring and read back `pd_len_` samples later. The dry never sees this ring.
3. **Read the 8 delay lines** (`:139-145`): one tap each at the current length `len`.
4. **Householder mix** (`:147-152`): `y_k = x_k − (2/8)·Σx` — one sum, eight subtractions, lossless.
5. **Feedback write-back** (`:154-169`): each mixed output is scaled by its per-line RT60 gain, passed through a one-pole damping LPF, then the pre-delayed send is injected (`in_gain_ = 0.5`, `:305`) — even lines fed from L, odd from R (`:162`) — and written to the ring with a branch-wrap instead of a modulo (more on that below).
6. **Decorrelated stereo wet** (`:171-186`): `wL`/`wR` are two different ±1-signed combinations of the 8 line outputs (scaled by `kTapNorm = 1/8`), crossfaded against the plain mono sum `wMono` by Width.
7. **Blend** (`:188-190`): `out = dry + mix · wet_norm · wet`. The dry sample the user gave you is the dry sample they get back, at unity, same sample index.

Internally everything is `double`; only the block edges are `float`. Note the damping filter sits on the *feedback* term only — the injected input's first pass through a line is undamped, and each recirculation accrues one more damping pass. That is exactly how physical air absorption works: loss per bounce, not loss at the door.

## The FDN core

### Why an FDN and not convolution

A convolution reverb ships a fixed room (an IR file) with a fixed colour and costs a partitioned FFT. A feedback delay network — the Stautner–Puckette/Jot lineage — is a handful of delay lines cross-coupled through a feedback matrix. With the right matrix and lengths it produces a dense, smooth, *colourless* exponential tail for a few dozen operations per sample, parametric in decay time, and with no file to ship. Transparency is the entire point of this stage, so the FDN wins (`reverb.hpp:12-16`).

### Losslessness: the Householder reflection

The feedback matrix is `H = I − (2/N)·𝟙𝟙ᵀ` with N = 8 — a Householder reflection about the unit vector `u = 𝟙/√N`, since `H = I − 2uuᵀ`. Two lines of algebra show it is orthogonal:

```
H² = I − (4/N)J + (4/N²)J²      where J = 𝟙𝟙ᵀ, and J² = N·J
   = I − (4/N)J + (4/N)J = I
```

So `HᵀH = I` and `‖Hx‖ = ‖x‖`: the mixing step *preserves energy exactly*. That matters for two reasons:

- **Decay control.** Because the matrix loses nothing, the tail's decay rate is set *entirely* by the per-line gains `fb_gain_[k]` and the damping filters. Every eigenmode of the loop decays at the designed rate — no mode rings on after the others (metallic resonance), no mode dies early (a hole in the tail). A lossy or non-normal matrix makes decay mode-dependent, which is precisely the "cheap reverb colour" this stage refuses.
- **Cost.** A dense 8×8 matrix is 64 multiplies per sample. The Householder form is one sum and eight subtract-multiplies — O(N):

```cpp
double sum = 0.0;
for (int k = 0; k < kLines; ++k) sum += out[k];
const double hf = (2.0 / kLines) * sum;
// per line: fb = (out[k] - hf) * fb_gain_[k];       reverb.hpp:149-158
```

And despite the O(N) form it still scatters every line into every other line each pass, which is what builds echo density.

### Prime delay lengths: no coincident echoes

The eight base lengths (`reverb.hpp:281-283`) are all prime, hence pairwise coprime:

```
443   587   691   829   947   1109   1303   1481   samples @ 44.1 kHz
10.0  13.3  15.7  18.8  21.5  25.1   29.5   33.6   ms
```

An FDN's impulse response contains echoes at every integer combination of the line delays. If lengths share divisors (or are near-rational multiples), arrival times pile up on a common period — audible as flutter and comb colouration. Coprime lengths push the recurrence period of any coincidence out to the LCM, i.e. effectively never, keeping the modal density smooth. This is the oldest rule in the Schroeder playbook and the header applies it literally (`:278-280`, "Primes avoid coincident echo periods (= flutter)").

At other sample rates the lengths are rescaled by `sr/44100` and rounded (`:214-217`), keeping the delays constant in *milliseconds* — so the room doesn't change size with the host's sample rate. (Rounding means the scaled lengths aren't literally prime at 96 kHz; the ratios stay incommensurate enough. See Limitations.)

### RT60: one knob, calibrated decay

Size maps linearly to a reverberation time (`:226`): `RT60 = 0.4 s + size · 1.6 s` (`kRt60Min`/`kRt60Max`, `:270-271`) — a small tasteful room up to a large one. Each line then gets its own feedback gain from the standard derivation: a line of delay `dt = len/sr` seconds recirculates `t/dt` times in `t` seconds, so requiring 60 dB of loss at `t = RT60`:

```
g^(RT60/dt) = 10^(−60/20)    ⟹    g = 10^(−3·dt/RT60)        reverb.hpp:227-231
```

The per-line exponent is the point: longer lines get *smaller* gains so that **every line decays at the same dB-per-second**. That, plus the lossless matrix, is what makes the tail a single clean exponential instead of eight competing ones.

Size actually drives three couplings at once in `design_()`:

- delay lengths scale ×1.0 … ×3.5 (`kMaxSizeScale`, `:215-217`) — bigger room, sparser early echo pattern;
- RT60 scales 0.4 → 2.0 s — bigger room, longer tail;
- pre-delay scales 8 → 30 ms (`kPredelayMinMs`/`kMaxPredelayMs`, `:246-250`) — bigger room, later first reflection.

One knob, physically coherent behaviour, no way to dial a "big room with a tiny predelay" inconsistency.

### Stability: provable, with margin to spare

The loop gain of line `k` is `fb_gain_[k] · |D(e^jω)|` where `D` is the damping one-pole. Two facts make the network unconditionally stable:

1. **The damping filter can never add gain.** It is `z += a·(x − z); y = z` with `a = 1 − exp(−2π·fc/sr)` (`:160`, `:243`). Its transfer function is `D(z) = a / (1 − (1−a)z⁻¹)`, whose magnitude peaks at DC where it is *exactly* `a / (1 − (1−a)) = 1`, and is strictly below 1 everywhere else for `0 < a < 1`. The design comment makes the invariant explicit (`:240-242`): unity DC gain means damping only ever *removes* loop energy. A damping filter with even +0.1 dB of passband ripple inside an FDN loop is a slow-motion explosion; this topology cannot express that bug.
2. **The feedback gain is capped**: `if (g > 0.9995) g = 0.9995` (`:233`). Combined with (1), loop gain < 1 on every line at every frequency, and since the Householder mix is energy-preserving, the whole network is BIBO stable by construction.

How much margin? Work out where `g` actually lands: `g = 10^(−3·dt/RT60)` is maximised when `dt/RT60` is smallest. `dt` is sample-rate-invariant (lengths scale with `sr`), and the ratio `(1 + 2.5·size)/(0.4 + 1.6·size)` is monotonically *decreasing* in size, so the worst case over the whole legal parameter space is the shortest line at Size = 1: `dt = 35.2 ms`, `RT60 = 2 s`, `g = 10^(−0.0528) ≈ 0.885`. The 0.9995 cap **never binds in normal operation** — it is a pure belt-and-braces guard against degenerate configurations (e.g. the `len < 2 → len = 2` clamp at absurd rates, `:218`). Defaults land even lower: at Size 0.30 the gains span ≈ 0.63 (longest line) to 0.87 (shortest).

The 10-second worst-case test (`test_reverb.cpp:123-144`: Size 1.0, Damp 18 kHz, i.e. maximum RT and minimum damping) confirms: bounded output, finite everywhere, last-second RMS < 1e−3.

## Width without mono risk

The lazy way to make a stereo tail is `R = −L`. It sounds huge on headphones and disappears — completely — on a mono speaker, because `L + R = 0`. This stage's refusal (`reverb.hpp:284-286`: "R is NOT the negation of L … they differ in pattern, not global sign") is built from tap-row algebra:

```cpp
kTapL = { +1, +1, +1, +1, -1, -1, -1, -1 };     // reverb.hpp:287-289
kTapR = { +1, -1, +1, -1, +1, -1, +1, -1 };     // reverb.hpp:290-292
```

These are two distinct rows of an 8×8 Hadamard-type basis, and three dot products tell the whole story (line outputs are mutually decorrelated to good approximation, thanks to the prime lengths):

- `kTapL · kTapR = 0` — L and R wet are **decorrelated**, which is what "width" *is*;
- `kTapL · 𝟙 = kTapR · 𝟙 = 0` — both wide channels are orthogonal to the mono sum row;
- `kTapL + kTapR = (2,0,2,0,0,−2,0,−2) ≠ 0` — the mono fold-down keeps half the energy (−3 dB, the normal incoherent-sum penalty), rather than cancelling.

Width then crossfades the wet pair against the plain mono sum (`:183-185`): `outWet = (1−width)·wMono + width·w{L,R}`, so Width 0 is a legitimately narrow (L = R) tail and Width 1 is fully decorrelated. Both `wL/wR` (scaled by `kTapNorm = 1/8`, `:181`) and `wMono` (scaled by `1/8`, `:182`) carry the same expected energy for uncorrelated line outputs, so sweeping Width doesn't pump loudness either.

Decorrelation is seeded at the *input* too: the mid-dominant send keeps ±0.5 of the side signal per channel (`:126-127`), and even lines are injected from L, odd from R (`:162`) — so even a mono input develops a stereo tail (verified: `test_reverb.cpp` test 5 feeds L == R, asserts `max|L−R| > 1e-3` *and* that the mono fold-down keeps > 0.3× channel RMS).

## Loudness discipline

Loudness is the mastering engineer's currency, and a reverb that changes it silently poisons every judgement made downstream. Three mechanisms hold it:

- **Dry at unity, always.** The blend is `out = dry + mix·wet_norm·wet` (`:188-190`) — parallel, never a crossfade. The dry signal is not scaled by `(1−mix)` and not delayed.
- **Wet normalisation across Size/Damp.** A bigger room rings longer, and a longer-ringing network accumulates more steady-state energy (for a single comb the buildup goes like `1/(1−g²)`). To stop Mix from meaning something different at every Size, the wet is trimmed by `wet_norm = 0.85/(1 + 0.6·size)` (`:264`, `kWetBase :274`): 0.85 at Size 0, 0.72 at the default 0.30, 0.53 at Size 1. The comment is honest that this is an *empirical, gentle taper* ("approx energy", `:259-263`), not an exact energy solve.
- **Bit-identical bypass.** `if (mix_ <= 0.f) return;` (`:114`) — not "quiet", not "−inf dB wet": the buffer is untouched. `test_reverb.cpp` test 1 asserts `max|out−in| == 0.0` exactly. This is also what makes the chain's isolation tests possible — `test_ssl_integration.py:23` sets `RVB_MIX=0` in its COMMON param string and can trust the stage contributes nothing.

## Engineering notes

### RT safety and the redesign gate

`prepare()` sizes every ring to the maximum it can ever need — base length × `kMaxSizeScale` × `sr/44100` + slack (`reverb.hpp:63-74`) — so `design_()` only ever *re-points* `len` within an existing buffer (`:218-220` clamps rather than reallocating). `process()` performs zero allocation. Total state is small: Σ base lengths = 7,390 samples, ×3.5 in doubles ≈ 210 KB for the lines plus ~21 KB of pre-delay at 44.1 kHz.

`set_params()` (`:97-110`) is called every block by the plugin, so it gates the expensive part: `mix` and `width` are stored unconditionally (they're per-sample multipliers), but `design_()` — lengths, RT gains, damping coefficient, HPF biquads, pre-delay, wet norm — runs only when Size/Damp/LowCut actually change. `prepare()` clears state *first* and designs *second* (`:76-77`), so a redesign never runs against stale line state.

### The branch-wrap micro-opt (June 2026 DSP pass, commit `8b9bd4e`)

The FDN *read* path always wrapped indices with a compare (`rp < 0 ? rp + size : rp`, `:141-143`), but the write path originally used `widx = (widx + 1) % buf.size()`. `buf.size()` is not a compile-time constant, so that modulo lowers to a real integer division — 8 of them per sample, plus one more on the pre-delay ring. Since the write index only ever advances by 1, a compare-and-reset is *exactly* equivalent:

```cpp
// Branch-wrap (matches the read path above) instead of a runtime
// `% buf.size()` — removes 8 integer divisions per sample. The
// index only ever advances by 1, so the compare is exact.
if (++line_[k].widx == static_cast<int>(line_[k].buf.size()))
    line_[k].widx = 0;                                   // reverb.hpp:164-168
```

Measured (Release -O3, Apple Silicon, 48 kHz stereo): **13.85 → 13.14 ns/sample**, null-tested `max|new−old| = 0.000e+00` — bit-identical. The commit message is explicit that this was consistency, not a hot-spot fix ("Reverb is only ~0.1% of a 128-blk budget"). The same pass added the guard that keeps it honest forever: `test_regression_kat` (`test_reverb.cpp:234-263`) locks a known-answer stereo render (Mix 0.4 / Size 0.6 / Width 0.7 / Damp 6 kHz / LowCut 200 Hz, 8192 samples of 220/330 Hz sines) to per-channel RMS within 1e−9 and four probe samples per channel within 1e−5, bit-stable across -O2/-O3.

### Denormals: checked, not needed

`bench_reverb.cpp` includes a burst-then-silence scenario that toggles FTZ via FPCR bit 24 on aarch64 (`bench_reverb.cpp:11-15`, `:35-39`) to see whether the decaying tail stalls in subnormal arithmetic. Result (from the perf-pass): FTZ on and off time identically — Apple Silicon flushes subnormals at full speed, so no denormal guard is needed. (A finding that generalised across the whole DSP pass; worth re-checking if an x86 build ever matters.)

### Well-conditioned: the null test actually works here

Unlike Auto-EQ and MelLimiter — which amplify 1-ULP build-level FP differences to −75…−90 dBFS and cannot pass a two-binary null test even for algebraically identical refactors — the Reverb is numerically *well-conditioned*: linear, smooth, no branches on data, no FFT alignment sensitivity. Two-binary null tests against it null to **exactly 0.0**. Any future refactor of this file should be held to full bit-identity; there is no excuse for a tolerance here.

## Plugin integration

- **Stage identity**: `StageID::Reverb = 8` (`axon_plugin.cpp:115`), a **single stereo instance** on the Plugin — not per-channel-chain (`:695-696`), because the FDN itself is the stereo processor. Prepared at activation (`:1234`), cleared on `plugin_reset` (`:1287`).
- **Chain placement** (`:711-715`): default order BassMono → SslEq → AutoEQ → **Reverb** → Widener → SslComp → MelLimiter. The reverb sits *after* BassMono so the bass feeding its send is already tightened, and *before* the limiter so reverb peaks are still caught. All stages are user-reorderable, but the default encodes the rationale.
- **The stage case** (`:2065-2076`) gates on `amt.rvb_mix <= 0.f` and `break`s before even calling `set_params` — bypass costs nothing. Otherwise it forwards the five resolved controls and processes the 128-sample work block in place (`kBlockSize = 128`, `axon_limits.hpp:18`), passing `nullptr` for R on mono buses (`reverb.hpp` handles `r == nullptr` throughout).
- **Controls** (the meta ↔ C++ contract): `RVB_MIX`, `RVB_SIZE`, `RVB_WIDTH`, `RVB_DAMP` (2–18 kHz), `RVB_LOWCUT` (20–1000 Hz) are declared in `axon/export/composite.py:161-167` (re-composed into `axon_meta.json` every build), read in `resolve_amount_` (`axon_plugin.cpp:1419-1421`) into the `AmountSnapshot` (`:1461-1465`), and *also* injected at load time with fallback specs for bundles that don't declare them (`:2786-2790`). The UI module is `REVERB` (`ui/index.html:503`), with `RVB_MIX` as the wet-param that lights the chain-strip activity dot.

### A defaults drift worth knowing about

The stage shipped (commit `3e5428b`) with `RVB_MIX` default **0.0** — reverb off, bit-identical, opt-in. Commit `4367cc4` ("default chain" tuning) flipped the bundle default to **1.0**, so out of the box the shipped chain runs the reverb at full Mix (Size 0.30 → wet trim ≈ 0.72). Two comments never caught up and are now stale on origin/main:

- `axon_plugin.cpp:2783-2785` still says "Defaults make the stage a no-op (RVB_MIX = 0 → bit-identical bypass)" directly above an injected spec whose default is `1.0f` (`:2786`). The *bypass semantics* (0 = bit-identical) remain true; the *default* claim does not.
- `native/clap/bench/run_bench.py:59-60` claims the `full_chain` scenario leaves the reverb off because "RVB_MIX … default 0" — but the scenario string (`run_bench.py:50`) doesn't set `RVB_MIX`, and the plugin initialises unset controls from bundle defaults (`axon_plugin.cpp:1073`), which is now 1.0. Scenario labels that assume "reverb off unless stated" should set `RVB_MIX=0` explicitly (as `test_ssl_integration.py:23` already does). The headline perf numbers below are unaffected — they come from `full_chain_all`, which sets `RVB_MIX=0.25` explicitly (`run_bench.py:68`).

## Measured

From the per-stage CPU ranking (`native/clap/docs/perf_stage_ranking.md`, sections 2 and 5, buf = 128, all-stages scenario, instrumented -O3 build):

| Metric | Value |
|---|---|
| Share of `process()` | **0.419%** (0.412% at buf = 512 — share is buffer-size-invariant) |
| Mean per block | 1.89 µs (68,900 calls) |
| p95 / max per block | 3.47 µs / 89.29 µs |
| Standalone throughput | ~13.1 ns/sample (`bench_reverb.cpp`, 48 kHz stereo, mix 0.3 size 0.5) |

The two numbers corroborate: 13.1 ns/sample × 128 samples ≈ 1.68 µs, right under the 1.89 µs measured in-chain. The ranking's verdict (section 5) is the correct one: **explicitly cheap — leave alone**; no optimisation of this stage clears any ROI bar while the ONNX stages own 90%+ of the budget.

The unit suite (`native/clap/tests/test_reverb.cpp`, compiled standalone with `g++ -O2 -std=c++17 -I src`) pins each refusal to an assertion:

| Test | Guarantee locked |
|---|---|
| `test_bypass_identity` | mix 0 → `max|out−in| == 0.0`, `latency_samples() == 0` |
| `test_tail_and_size` | a tail exists, decays, and −40 dB time grows with Size |
| `test_stability` | 10 s worst case (Size 1, Damp 18 kHz): finite, bounded (< 8.0), decays to < 1e−3 RMS |
| `test_lowcut` | 80 Hz wet energy < 0.25× of 1 kHz wet — bass genuinely not reverberated |
| `test_width_mono_compat` | mono input → decorrelated L/R, and mono fold-down keeps > 0.3× channel RMS |
| `test_no_dc` | output mean ≈ 0 (the send HPF also kills DC recirculation) |
| `test_regression_kat` | known-answer render, RMS 1e−9 + 4 probes/ch 1e−5, guards the branch-wrap change |

## Limitations and open ideas

- **No early-reflection model.** The tail onset is the pre-delay plus the shortest line; there is no discrete ER pattern before the diffuse field. For "space, width, depth at a distance" (the mastering use case) that's arguably a feature — ER patterns are the most room-signature, colour-carrying part of a reverb — but this will never image like a chamber/plate emulation.
- **Householder builds echo density slower than richer matrices.** A full ±1 Hadamard scatters with sign diversity and reaches perceptual density in fewer round trips; the rank-1-update reflection is smoother but slower to thicken. At mastering mix levels, behind 8–30 ms of pre-delay, this is inaudible; at Mix = 1 with tiny Size a careful ear might catch a slightly granular first 50 ms.
- **No delay-line modulation.** Classic FDNs often gently modulate line lengths to break residual periodicity. That adds pitch wobble — an immediate refusal on a master bus — so the tail is perfectly static. The prime lengths are doing all the anti-flutter work alone, and they hold up.
- **RT60 is nominal at DC.** The per-line gains realise the target RT60 exactly *only* at DC; the damping LPF adds extra HF loss per recirculation, so the effective mid/high RT is shorter than the Size knob's implied seconds whenever Damp is low. Intentional (that's what air absorption *is*), but the 0.4–2.0 s figures are ceilings, not measurements at 1 kHz.
- **Size automation is not click-free.** `design_()` re-points `line_[k].len` and `pd_len_` instantly; the read tap jumps to a different ring position, discontinuously changing the tail (and the damping/HPF coefficients step too). Mix and Width are safe to automate (pure per-sample multipliers, updated per 128-sample block); Size/Damp/LowCut are set-and-forget mastering knobs. A crossfaded double-tap or interpolated-length scheme would fix it at real cost and was never needed.
- **A muted tail freezes rather than decays.** The `mix <= 0` early return stops all processing, so the line state is frozen; re-raising Mix minutes later resumes the *old* tail for a moment instead of silence. Trivially fixable (run the network at mix 0, or reset on re-enable) — but both alternatives were refused for good reasons (cost in the always-on chain; a reset would also click).
- **Scaled lengths lose primality off 44.1 kHz.** `lround(kBaseLen · sizeScale · sr/44100)` preserves the millisecond geometry, not the number theory. In practice the eight lengths remain mutually incommensurate enough that no flutter has been observed at 48/96 kHz, and the KAT pins 44.1 kHz only.
- **Mid-dominant send under-reverberates hard-panned material.** The send is 75/25 own/other per channel, so pure-side content enters the network ~6 dB down relative to mid. Deliberate (side-heavy reverb input widens the tail at the cost of centre-image stability) but worth knowing when judging wet level on wide mixes.
- **`wet_norm` is a taper, not a law.** `0.85/(1+0.6·size)` was tuned by ear; there is no closed-form energy normalisation across Size × Damp × material. If anyone ever revisits it, note it interacts with the stale-default issue above (default chain currently runs Mix = 1.0).

## Sources

- `native/clap/src/reverb.hpp` (origin/main; all line anchors above) — the header comment is the design manifesto and remains the primary written source.
- `native/clap/src/biquad.hpp:19-33, 59-75` — shared TDF2 biquad + RBJ Butterworth HPF used by the send.
- `native/clap/src/axon_plugin.cpp:115, 695-696, 711-715, 1234, 1287, 1342-1345, 1377, 1419-1421, 1461-1465, 2065-2076, 2783-2790` — integration.
- `native/clap/tests/test_reverb.cpp`, `native/clap/bench/bench_reverb.cpp` — verification and measurement.
- `native/clap/docs/perf_stage_ranking.md` §2, §5 — in-chain cost (0.419%, 1.89 µs; leave alone). That doc remains authoritative for chain-wide performance; this dive does not supersede it.
- Commits: `3e5428b` (stage introduced), `4367cc4` (default chain: RVB_MIX 0 → 1.0), `8b9bd4e` (branch-wrap perf, bit-identical, KAT added), `c208fdd` (biquad extraction to shared header).
