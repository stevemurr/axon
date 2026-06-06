# LUFS Leveler

> A BS.1770-based **automatic loudness leveler**: it continuously measures how loud the audio is (in LUFS) and rides a smoothed gain up or down to pin the signal at a chosen target loudness.

---

## ⚠️ Status: legacy / NOT in the active signal chain

**This module is not wired into the shipping Axon plugin.** The Input/Output Levelers
were removed from the signal chain, and `axon_plugin.cpp` no longer references
`LufsLeveler` (verified: `grep -i lufs_leveler axon_plugin.cpp` returns nothing).

The source still lives in the tree:

- `native/clap/src/lufs_leveler.hpp`
- `native/clap/src/lufs_leveler.cpp`

…and it is still **compiled and unit-tested** — but only as a standalone reference.
It is built into the `test_meter` target (`native/clap/CMakeLists.txt:132`, alongside
`src/lufs_leveler.cpp`) and exercised by `tests/test_dsp.cpp`, where it doubles as an
independent LUFS probe to cross-check the live `LoudnessMeter`.

Think of this document as describing the **conceptual ancestor** of today's
**Loudness Meter + Auto Gain** pair. The DSP below is accurate and worth
understanding, but if you are looking for what actually runs in the plugin, see the
[Relationship to current modules](#9-relationship-to-current-modules) section.

---

## 1. What is this?

### The everyday problem

Different songs (or different moments inside one song) are at very different volumes.
A quiet acoustic intro and a wall-of-sound chorus can be 15 dB apart. If you want
**everything to sit at one consistent loudness** — say, the streaming standard of
−14 LUFS — you'd normally have to grab a fader and ride it by hand, turning up the
quiet bits and turning down the loud bits.

A **loudness leveler** automates exactly that fader. It listens to how loud the audio
*actually is right now*, compares that to a **target loudness** you've chosen, and
smoothly turns the volume up or down to close the gap. Quiet → boosted. Loud →
pulled back. The goal is that the *average perceived loudness* stays glued to the
target.

> **Analogy:** It's the cruise control of volume. You set a target speed (target
> LUFS); the system watches your actual speed (measured LUFS) and feathers the
> throttle (gain) to hold it there — gently, not by slamming the pedal.

### What is "LUFS"?

**LUFS** = *Loudness Units relative to Full Scale*. It's a measurement of **perceived
loudness**, standardized in **ITU-R BS.1770** (and used by EBU R128). Unlike a raw
peak or RMS meter, LUFS applies a **K-weighting** filter that mimics how human hearing
responds to different frequencies (we're more sensitive to mids/highs than deep bass),
then averages the energy over a time window. The result correlates far better with
"how loud does this *sound*" than a peak meter does. Streaming platforms normalize to
LUFS targets (Spotify/YouTube ≈ −14 LUFS), which is why −14 is the built-in default
here.

### How it differs from a compressor

This trips up beginners, so it's worth being explicit:

| | **Loudness Leveler (this module)** | **Compressor** |
|---|---|---|
| Reacts to | **Average loudness over seconds** (3 s window) | **Instantaneous level**, sample-by-sample / ms |
| Time scale | Slow: attack 50 ms, release 500 ms | Fast: attack µs–ms, release ms |
| What it changes | A single broadband **make-up gain** that drifts | The **internal dynamics** — squashes peaks relative to valleys |
| Effect on dynamics | Largely **preserves** them (whole signal moves together) | **Reduces** them (loud bits squashed toward quiet bits) |
| Mental model | An automated volume fader | A fast automated *attenuator that only ducks loud peaks* |

A compressor changes the *shape* of the loud-vs-quiet relationship within a passage.
A leveler leaves that shape alone and just slides the *whole passage* toward a target
volume. The leveler is slow on purpose — it should not "pump" on individual drum hits.

### How it differs from the current Auto Gain

The plugin's current **Auto Gain** (`src/auto_gain.hpp`) is a narrower, simpler idea
called **level-matched bypass** (a.k.a. "gain match"). Its only job is to make the
*processed output* the **same loudness as the input** so you can A/B the effect of the
chain honestly ("louder always sounds better," so it cancels that bias). It does **not**
aim for an absolute target like −14 LUFS — it aims at *whatever the input loudness is*.

The LUFS Leveler is more ambitious: it targets an **absolute, user-chosen LUFS value**
regardless of how loud the input was.

---

## 2. Why it mattered / historical role

In the early architecture, Axon had explicit **Input** and **Output Levelers** built
on this class. The idea was:

- **Input Leveler:** normalize whatever the user fed in to a consistent loudness
  *before* the neural mastering model and the rest of the chain saw it. Neural models
  behave best when the input sits in a predictable loudness range, so leveling at the
  front made the model's job more consistent.
- **Output Leveler:** pin the final master to a streaming-ready target (−14 LUFS) so
  the plugin could act as a "set it and forget it" loudness-normalizer at the end of
  the chain.

This is a genuinely useful, self-contained piece of DSP: a from-scratch **BS.1770-4
short-term LUFS meter** plus an **attack/release gain rider**, with **no dependency on
CLAP, ONNX Runtime, or libebur128** (see the header comment, `lufs_leveler.hpp:1-6`).
That independence is exactly why it survives as a unit-testable reference even after
being pulled from the chain.

---

## 3. The DSP behind it

Two engines run in series:

1. A **loudness estimator** (BS.1770-4 LUFS) that produces a number like "−18.3 LUFS".
2. A **gain controller** that turns the gap between measured and target loudness into a
   smoothly-rising/falling multiplier applied to the audio.

### 3a. K-weighting (BS.1770-4)

LUFS is not raw energy — it is energy measured through a **K-weighting** filter, which
is two cascaded biquad (2nd-order IIR) filters:

1. A **pre-filter** — a high-shelf that boosts highs ~+4 dB, modeling the acoustic
   effect of the head.
2. An **RLB filter** — a 2nd-order high-pass ("Revised Low-frequency B-curve") that
   rolls off very low bass, which we perceive as less loud.

The exact coefficients are tabulated from the BS.1770-4 spec and **match the
libebur128 reference implementation** (`lufs_leveler.cpp:10-12`). There are exact
constants for the two common rates:

**48 kHz** (`lufs_leveler.cpp:20-25`):

```cpp
static constexpr KCoeffs kCoeff_48000{
    1.53512485958697,  -2.69169618940638,  1.19839281085285,   // pre  b0,b1,b2
    -1.69065929318241,  0.73248077421585,                       // pre  a1,a2
    1.0,               -2.0,               1.0,                  // rlb  b0,b1,b2
    -1.99004745483398,  0.99007225036621,                       // rlb  a1,a2
};
```

**44.1 kHz** (`lufs_leveler.cpp:27-32`):

```cpp
static constexpr KCoeffs kCoeff_44100{
    1.5308412300503478, -2.6509799000031379, 1.1690790340624427,
    -1.6636551132560902, 0.7125954280732254,
    1.0,                 -2.0,                1.0,
    -1.9891696736297957, 0.9891959257876969,
};
```

Each biquad runs in **Transposed Direct Form II** (`Biquad::step_l/step_r`,
`lufs_leveler.hpp:68-79`) with **separate left/right state registers** (`z1_l, z2_l,
z1_r, z2_r`) so one shared leveler instance can K-weight a linked stereo pair without
the two channels contaminating each other's filter memory.

> **Vet note — non-standard sample rates:** for rates other than 44.1/48 kHz there are
> no tabulated coefficients, so `warp_biquad_to_sr()` (`lufs_leveler.cpp:38-52`)
> applies a crude proportional z-plane warp from the 48 kHz prototype
> (`scale = src_sr/dst_sr`, multiplying `b1,a1` by `scale` and `b2,a2` by `scale²`).
> The code is explicit that this is **not** the spec-correct continuous-time redesign
> and "degrades accuracy at extreme rate differences" — it's tolerated only because the
> result is integrated over a 3-second window that smooths the error
> (`lufs_leveler.cpp:34-37, 43-45`). In practice the plugin runs at 44.1/48 kHz, so the
> exact path is what's used.

### 3b. From K-weighted energy to LUFS

Per BS.1770, loudness is computed from the **mean-square** of the K-weighted signal:

```
L_k = -0.691 + 10 · log10(mean_square)
```

This is implemented verbatim (`lufs_leveler.cpp:122-126`):

```cpp
static inline double lufs_from_ms(double ms) {
    // BS.1770: L_k = -0.691 + 10 log10(ms)
    if (ms <= 0.0) return -120.0;
    return -0.691 + 10.0 * std::log10(ms);
}
```

The `−0.691` is the standard BS.1770 absolute calibration offset. (Note this is
`10·log10`, not `20·log10`, because `ms` is already squared — i.e. it's power.)

### 3c. The short-term (3 s) window — a ring of 100 ms sub-blocks

The "short-term" loudness in BS.1770 is the loudness over the **last ~3 seconds**. The
code computes this efficiently with a **ring buffer of 100 ms sub-blocks**:

- `kSubBlockMs = 100` (`lufs_leveler.cpp:54`) — each sub-block is 100 ms.
- The window is `cfg_.short_term_s = 3.0` s (`lufs_leveler.hpp:29`), so the ring holds
  `ceil(3000/100) = 30` sub-blocks (`ring_blocks_`, `lufs_leveler.cpp:92-94`).

Within a sub-block, K-weighted samples are squared and accumulated
(`sub_block_sum_sq_`). At each 100 ms boundary the sub-block's mean-square is committed
into the ring, and an **incrementally-maintained running sum** (`ring_sum_ms_`) is
updated by subtracting the value being evicted and adding the new one
(`lufs_leveler.cpp:146-147`). That makes the short-term loudness an **O(1)** update — no
re-summing 30 blocks every time:

```cpp
double ms = sub_block_sum_sq_ / static_cast<double>(sub_block_fill_);
ring_sum_ms_ += ms - ms_ring_[ring_idx_];   // add new, subtract evicted
ms_ring_[ring_idx_] = ms;
ring_idx_ = (ring_idx_ + 1) % ring_blocks_;
if (ring_filled_ < ring_blocks_) ++ring_filled_;

double window_ms = ring_sum_ms_ / static_cast<double>(ring_filled_);
```

`ring_filled_` ramps from 0 to 30 after a reset, so the window divides by *actual* data
present rather than waiting a full 3 s before producing a reading — the leveler starts
working almost immediately, just with a shorter effective window at first.

> **Note (BS.1770 fine print):** the spec's short-term measurement uses *overlapping*
> 100 ms blocks averaged over a sliding 3 s. This implementation uses *non-overlapping*
> 100 ms sub-blocks and a flat (rectangular) mean over them — a slightly coarser but
> standard simplification. Loudness *gating* (the −70/−10 LUFS relative gate used for
> *integrated* loudness) is **not** applied; this is a short-term measurement, plus a
> simple absolute silence gate (below).

### 3d. Target-loudness gain computation

Once a short-term LUFS reading exists, the desired gain is just the gap between target
and measured loudness, **expressed in dB and clamped** (`lufs_leveler.cpp:154-157`):

```cpp
last_lufs_ = lufs_from_ms(window_ms);
double delta_db = target_lufs_ - last_lufs_;          // how far we must move, in dB
if (delta_db > cfg_.max_gain_db) delta_db = cfg_.max_gain_db;   // +12 dB cap
if (delta_db < cfg_.min_gain_db) delta_db = cfg_.min_gain_db;   // -12 dB cap
target_gain_lin_ = std::pow(10.0, delta_db / 20.0);   // dB → linear multiplier
```

So if the music measures −20 LUFS and the target is −14 LUFS, `delta_db = +6` → the
target gain is `10^(6/20) ≈ 2.0×`. If it's too loud, `delta_db` is negative and the
target gain is below 1.0 (attenuation). The gain is **clamped to ±12 dB**
(`min_gain_db = -12`, `max_gain_db = +12`, `lufs_leveler.hpp:31-32`) so the leveler can
never push more than 12 dB in either direction — important so it doesn't try to amplify
a near-silent passage by 40 dB.

This target gain is recomputed **once per 100 ms sub-block**, not per sample.

### 3e. Silence handling

If the audio is essentially silent, you do **not** want the leveler to slam +12 dB of
gain onto the noise floor (or onto a fade-out tail). A **silence gate** prevents this:

- `silence_floor_dbfs = -70.0` (`lufs_leveler.hpp:36`) → converted to a mean-square
  threshold `silence_ms = 10^(-70/10)` (`lufs_leveler.cpp:131`).
- Each sub-block, the LUFS/target-gain update is **skipped** if the window's mean-square
  is below that floor (`lufs_leveler.cpp:152`, `if (window_ms >= silence_ms)`). On
  silence the target gain is simply **left where it was** — no boost-the-silence
  pathology. The `test_lufs_silence` test asserts the gain stays within ±0.5 dB of unity
  after 5 s of pure silence (`tests/test_dsp.cpp:109-122`).

### 3f. Gain smoothing (attack / release)

A target gain that jumps every 100 ms would cause audible zipper noise / pumping.
Instead the actual applied gain **rides toward** the target with a **per-sample
one-pole smoother**, with different speeds for going up vs. down.

The coefficients are computed in `reset()` from time constants
(`lufs_leveler.cpp:105-112`):

```cpp
// y[n] = a*y[n-1] + (1-a)*x[n];  a = exp(-1 / (tau_s * fs))
auto pole = [&](double ms) {
    double tau_s = std::max(ms, 1e-3) * 1e-3;
    return std::exp(-1.0 / (tau_s * sample_rate));
};
attack_coeff_  = pole(cfg_.attack_ms);    // 50 ms
release_coeff_ = pole(cfg_.release_ms);   // 500 ms
```

with `attack_ms = 50.0` and `release_ms = 500.0` (`lufs_leveler.hpp:30-31`).

> **Naming caution:** here **"attack"** is the time constant used when the gain is
> *increasing* toward target (`target > current`), and **"release"** when *decreasing*.
> Because the gain itself can be a boost or a cut, "attack = gain rising" can mean the
> level goes *up* (boosting a quiet part). This is the opposite of compressor "attack"
> (which clamps level *down*). The faster 50 ms attack lets it catch up quickly when it
> needs more gain; the slower 500 ms release backs gain off gently.

The smoother runs **every sample** (`lufs_leveler.cpp:166-170`):

```cpp
double coeff = (target_gain_lin_ > smooth_gain_lin_) ? attack_coeff_ : release_coeff_;
smooth_gain_lin_ = coeff * smooth_gain_lin_ + (1.0 - coeff) * target_gain_lin_;
if (smooth_gain_lin_ > tgt_lin_max) smooth_gain_lin_ = tgt_lin_max;   // safety clamp
if (smooth_gain_lin_ < tgt_lin_min) smooth_gain_lin_ = tgt_lin_min;
out[i] = static_cast<float>(x * smooth_gain_lin_);
```

The final clamps (`tgt_lin_max`/`tgt_lin_min` = ±12 dB linear,
`lufs_leveler.cpp:129-130`) are a belt-and-suspenders guard against numeric drift; the
gain is already bounded because the *target* is bounded.

> **Important — the loop is open, not closed.** The leveler measures the **input**
> (it K-weights `in[i]` before applying gain) and applies the resulting gain to that
> same input. It does **not** re-measure its own output. So it computes an open-loop
> feed-forward correction: "the input is X dB from target, so multiply by that gap."
> Because gain in dB is linear, applying `(target − measured)` dB to the input lands
> the output's loudness near the target in one shot (verified by `test_lufs_converges`
> and `test_lufs_attenuates_loud`, which both assert the *output* LUFS settles within
> 2 dB of −14, `tests/test_dsp.cpp:60-107`).

---

## 4. How it works in the code — walkthrough

### Construction & config

`LufsLeveler` is default-constructible or takes a `Config`
(`lufs_leveler.hpp:40-41`). All tunables live in `Config` (`lufs_leveler.hpp:26-37`):
target LUFS, short-term window, attack/release, gain clamps, silence floor.

### `reset(sample_rate, target_lufs)` — `lufs_leveler.cpp:86-115`

Call this on `activate()` (or any time to re-zero state mid-stream). It:

1. Stores `sample_rate_` and `target_lufs_`.
2. Loads K-weighting coefficients via `set_k_weighting_coeffs_()`
   (`lufs_leveler.cpp:58-84`), which picks the exact 44.1/48 kHz table or warps from
   48 kHz, then clears the biquad state.
3. Sizes the ring: `sub_block_samples_ = 100 ms · fs`, `ring_blocks_ = 30`, and
   `ms_ring_.assign(30, 0.0)` (`lufs_leveler.cpp:91-94`). **This is the only
   allocation** — `process()` allocates nothing.
4. Zeroes all accumulators and sets both `smooth_gain_lin_` and `target_gain_lin_` to
   **1.0** (unity).
5. Computes `attack_coeff_` / `release_coeff_`.

### `set_target(target_lufs)` — `lufs_leveler.hpp:47`

Changes the target loudness **without** resetting filter/gain/ring state — for live
knob moves. The new target takes effect at the next sub-block boundary.

### `process(in, out, n)` — `lufs_leveler.cpp:128-174` (mono)

The hot loop. For each sample: K-weight a copy → accumulate squared energy → on a
100 ms boundary commit to the ring, recompute LUFS + target gain (unless gated by
silence) → one-pole-smooth the gain → multiply and write. `out` may alias `in`
(in-place is allowed).

### `process_linked(lin, rin, lout, rout, n)` — `lufs_leveler.cpp:176-221` (stereo)

Identical structure, but K-weights both channels (using the separate `step_l`/`step_r`
state registers) and uses the **BS.1770 channel-sum** energy `L² + R²` for the loudness
estimate (`lufs_leveler.cpp:187-189`). One shared gain ride is derived from the combined
loudness and applied **equally to both channels** — so stereo imaging is preserved and
the channels never drift in level relative to each other.

> **Vet note on the stereo MS normalization:** `sub_block_sum_sq_` accumulates
> `L²+R²` but is divided by `sub_block_fill_` (the *sample-pair* count, not 2× that),
> so the mean-square is per *pair* rather than per *channel*. This matches the BS.1770
> convention of summing channel powers (each channel weighted 1.0 for L/R), but it
> means a stereo signal reads ~3 dB "louder" in mean-square than the same content
> summed to mono — by design, consistent with the standard's channel weighting.

### Diagnostics

- `last_measured_lufs()` (`lufs_leveler.hpp:58`) — the most recent short-term LUFS
  reading; used by the tests as an independent LUFS probe.
- `current_gain_db()` (`lufs_leveler.cpp:117-120`) — the live applied gain in dB
  (`20·log10(smooth_gain_lin_)`), returning `min_gain_db` if gain is ≤ 0.

---

## 5. Latency & performance

- **Latency:** **zero added latency** in the signal path. Gain is computed and applied
  to the same sample in the same iteration; there is no look-ahead and no delay line.
  (The 3 s window is a *measurement* window, not a delay — the leveler simply *reacts*
  over ~seconds; it never *delays* the audio.)
- **Reaction time:** governed by the 3 s short-term window plus the 50 ms attack /
  500 ms release smoothing. It deliberately responds over hundreds of ms to seconds, so
  it tracks musical loudness without pumping on transients.
- **CPU:** very cheap and constant-cost. Per sample: 2 biquads (~10 mul-adds) + one
  multiply. The expensive bits (`log10`, `pow`) run **only once per 100 ms sub-block**,
  not per sample. The short-term average is O(1) thanks to the incremental ring sum.
- **Real-time safety:** all memory is allocated in `reset()`; `process()` /
  `process_linked()` do **no allocation, no locks, no I/O** — safe on the audio thread.
- All math is `double` internally; only the final write is cast to `float`.

---

## 6. Parameters

Defined in `Config` (`lufs_leveler.hpp:26-37`):

| Parameter | Field | Default | Meaning / range notes |
|---|---|---|---|
| **Target loudness** | `target_lufs` | **−14.0 LUFS** | The loudness it aims for. −14 is the streaming-era default (Spotify/YouTube). Live-settable via `set_target()`. Any value works; pair with the gain caps. |
| **Short-term window** | `short_term_s` | **3.0 s** | BS.1770 short-term integration window. Longer = steadier but slower to react. |
| **Attack** | `attack_ms` | **50 ms** | Smoothing time constant when gain is *rising* toward target. |
| **Release** | `release_ms` | **500 ms** | Smoothing time constant when gain is *falling* toward target. |
| **Min gain** | `min_gain_db` | **−12 dB** | Hardest attenuation allowed. |
| **Max gain** | `max_gain_db` | **+12 dB** | Hardest boost allowed. Total leveling range = 24 dB. |
| **Silence floor** | `silence_floor_dbfs` | **−70 dBFS** | Below this (mean-square), updates are skipped so silence/fade-tails don't trigger a boost. |

There is **no separate "speed"/"strength" knob** — leveling aggressiveness is shaped
by the window length + attack/release + the ±12 dB clamp. (A user-facing strength
control, had it shipped, would most naturally have mapped to attack/release or to a
fractional scaling of `delta_db`.)

---

## 7. Gotchas / things to watch

- **It's slow, by design.** It will not fix a sudden 10 dB jump instantly — expect
  hundreds of ms to a couple seconds to settle. That's correct behavior for a *loudness*
  leveler, not a bug.
- **±12 dB hard limit.** Material more than 12 dB from target will *not* reach the
  target — it tops out at the clamp. Watch `current_gain_db()` pinned at ±12.
- **Boost raises the noise floor.** Pushing a quiet section up +12 dB also pushes its
  hiss up +12 dB. No expander/gate is involved.
- **No look-ahead / no inter-sample-peak protection.** A boost can push samples past
  0 dBFS and clip. In the old chain this was paired downstream with a true-peak ceiling
  (`true_peak_ceiling.cpp`); standalone, the leveler offers no overshoot protection.
- **"Attack/release" are gain-direction terms,** not level-direction terms — see the
  caution in §3f. Easy to misread.
- **Non-44.1/48 kHz uses an approximate K-weighting** (the warp in §3a). Fine for
  loudness-control purposes; do not treat its LUFS readings at exotic rates as
  metering-grade.
- **Shared-instance state.** `process()` uses only the `_l` filter registers;
  `process_linked()` uses both. Don't mix the two calls on one instance without a
  `reset()`, or the right-channel filter state will be stale.
- **Open-loop assumption.** It corrects based on the *input* and trusts that applying
  the dB gap lands the output on target. For purely linear gain this is exact; if you
  inserted nonlinear processing between measurement and the multiply, the assumption
  would break (it doesn't here — measure and apply are the same sample).

---

## 8. Quick reference — usage

```cpp
nablafx::LufsLeveler lvl;
lvl.reset(44100.0, /*target_lufs=*/-14.0);   // on activate()
lvl.process(audio, audio, num_samples);       // in-place OK

// Linked stereo (one shared gain ride, imaging preserved):
nablafx::LufsLeveler stereo;
stereo.reset(48000.0, -16.0);
stereo.process_linked(L, R, L, R, n);

// Live target change (no state reset):
lvl.set_target(-9.0);

// Diagnostics:
double lufs = lvl.last_measured_lufs();
double g_db = lvl.current_gain_db();
```

---

## 9. Relationship to current modules

The LUFS Leveler was effectively **split into two smaller, more focused pieces**, and
its ideas live on in both:

### → `LoudnessMeter` (`src/meter.hpp`, `src/meter.cpp`) — the measurement half

The meter took over the **BS.1770 LUFS measurement** and generalized it. It uses the
**same K-weighting constants** as the leveler — the meter's header and source say so
explicitly (`meter.hpp:5-6`, `meter.cpp:10`: *"same constants as LufsLeveler"*) — and
the same 100 ms sub-block ring approach (`kSubMs = 100`, `kShortMs = 3000` → 30 blocks,
`meter.hpp:72-74`). It then adds what the leveler lacked:

- **Momentary (400 ms) LUFS** in addition to short-term (`kMomBlocks = 4`,
  `meter.hpp:74`).
- **Windowed RMS (dBFS)** and a **decaying sample peak**, for in/out metering.
- A clean `Readout` struct and a pure read-only `process()` (it never changes audio).

Crucially, the leveler is now the meter's **test oracle**: `tests/test_dsp.cpp` uses
`LufsLeveler::last_measured_lufs()` as an *independent* LUFS implementation to
cross-check the live `LoudnessMeter`, and they're co-compiled in the `test_meter`
target (`CMakeLists.txt:128-134`).

### → `AutoGain` (`src/auto_gain.hpp`) — the gain-riding half (scoped down)

`AutoGain` kept the spirit of "ride a smoothed gain from a LUFS measurement," but
**changed the goal and simplified the mechanism**:

- **Goal:** *level-matched bypass* — match the **output** loudness to the **input**
  loudness (`target = in_lufs − out_lufs`, `auto_gain.hpp:53`), so engaging the chain
  is loudness-neutral. It does **not** target an absolute −14 LUFS.
- **Mechanism:** it consumes LUFS readings **from the `LoudnessMeter`** (passed in as
  `in_lufs`/`out_lufs`) rather than measuring loudness itself. So the measurement and
  gain-riding responsibilities are cleanly separated.
- **Smoother:** a **per-block** one-pole (`kSmooth = 0.004f`, `auto_gain.hpp:65`) rather
  than the leveler's per-sample dual attack/release.
- **Feed-forward of a known gain** (e.g. the limiter Drive, `ff_db`,
  `auto_gain.hpp:47`) so monitoring level doesn't jump while the slow LUFS loop catches
  up — a refinement the leveler never had.
- Different gate (`kFloor = -50` LUFS) and a wider clamp (`kMaxDb = 24` dB).

### Why the leveler was removed

The Input/Output Levelers were pulled because:

1. **Concerns were entangled.** The leveler bundled measurement + gain control in one
   class. Splitting them into `LoudnessMeter` (measure, display) and `AutoGain`
   (decide gain) is cleaner and lets the UI show real in/out loudness.
2. **The product need changed.** A mastering plugin wants *honest A/B* (level-matched
   bypass) and a *visible meter* more than it wants a black-box auto-normalizer baked
   into the signal path. Absolute-target leveling inside the chain also fights the
   user's own loudness decisions and the downstream limiter.
3. **Risk in the path.** Open-loop ±12 dB gain with no look-ahead can clip; moving
   loudness handling to a *monitoring* trim (`AutoGain` multiplies after the meter tap)
   keeps the true master untouched.

It remains in the tree as a **standalone, dependency-free, unit-tested reference
implementation of BS.1770 short-term LUFS + an attack/release gain rider** — useful for
cross-checking the meter and as the documented ancestor of today's design.

---

## Appendix — source map

| Concept | Location |
|---|---|
| Class / `Config` | `native/clap/src/lufs_leveler.hpp:24-115` |
| K-weighting constants (48k / 44.1k) | `lufs_leveler.cpp:20-32` |
| Sample-rate warp fallback | `lufs_leveler.cpp:38-52` |
| `reset()` | `lufs_leveler.cpp:86-115` |
| LUFS-from-mean-square formula | `lufs_leveler.cpp:122-126` |
| Mono `process()` | `lufs_leveler.cpp:128-174` |
| Sub-block commit + target-gain calc | `lufs_leveler.cpp:144-163` |
| Per-sample gain smoothing | `lufs_leveler.cpp:166-170` |
| Stereo linked `process_linked()` | `lufs_leveler.cpp:176-221` |
| Unit tests | `native/clap/tests/test_dsp.cpp:51-122` |
| Built into | `test_meter` target, `CMakeLists.txt:128-134` |
| Successor: meter | `native/clap/src/meter.hpp`, `meter.cpp` |
| Successor: auto gain | `native/clap/src/auto_gain.hpp` |
