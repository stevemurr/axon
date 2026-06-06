# Loudness Meter (Axon DSP module)

A real-time, allocation-free meter that reports **LUFS** (short-term and momentary), **windowed RMS**, and a **decaying sample peak** for both the input and the output of the Axon mastering chain.

Source: [`native/clap/src/meter.hpp`](../native/clap/src/meter.hpp), [`native/clap/src/meter.cpp`](../native/clap/src/meter.cpp). Wiring: [`native/clap/src/axon_plugin.cpp`](../native/clap/src/axon_plugin.cpp).

---

## 1. What is this?

The Loudness Meter is the "how loud is this, really?" gauge. It does not change the sound at all — it only *listens* and produces four numbers per stream. Axon runs two of them: one on the audio that comes *in* and one on the master that goes *out*, so you can compare them honestly.

Three different ideas of "loudness" matter here, and they are genuinely different:

- **Peak** — the single highest sample value at any instant. Think of it as the tallest spike in the waveform. It tells you about *clipping headroom* (whether a sample will exceed 0 dBFS and distort), but it says almost nothing about how loud something *sounds*. A sharp transient can peak high yet sound quiet.
- **RMS (Root-Mean-Square)** — the average *power* of the signal over a short window. This is much closer to perceived loudness than peak, because your ears respond to sustained energy, not instantaneous spikes. RMS is the "thickness" of the sound.
- **LUFS (Loudness Units relative to Full Scale)** — the modern, standardized loudness measure defined by **ITU-R BS.1770**. It is essentially a fancy RMS with two refinements:
  1. **K-weighting** — the signal is filtered before measuring, to match how human hearing weights different frequencies (we are more sensitive to upper-mids than to deep bass or extreme highs).
  2. **Channel summing** — left and right are combined so a stereo mix reads consistently.

  The result is a number that lines up with how loud people actually *judge* a track to be. 0 LUFS is the theoretical maximum (full scale); real music sits well below, e.g. around -14 LUFS for streaming, -23 LUFS for broadcast.

LUFS comes in three integration windows (how long a span of audio it averages over):

- **Momentary** — a 400 ms window. Fast and twitchy; follows the music almost in real time.
- **Short-term** — a 3 s window. Smoother; good for "how loud is this section right now."
- **Integrated** — the loudness of the *whole* program from start to finish. (Axon's meter computes momentary and short-term; it does not gate-and-integrate over an entire song — see Gotchas.)

**Analogy.** Peak is the loudest *word* anyone shouts in a room. RMS is the *volume of conversation* over the last few seconds. LUFS is that same conversation volume, but measured through ears that are tuned like human ears (K-weighting) — which is why it tracks "how loud does the room *feel*" better than a raw sound-pressure reading.

---

## 2. Why it matters in mastering

Mastering is largely about hitting a **loudness target** without wrecking the dynamics or distorting. Streaming platforms normalize everything to a reference loudness (Spotify, Apple Music, YouTube all sit near -14 LUFS). If you master much louder than that, the platform simply turns you *down* — so you paid in dynamic range and got no loudness benefit. The LUFS meter is how you aim at that target deliberately instead of by feel.

The meter is also the backbone of **honest A/B level matching**. The single biggest trap in mastering is that *louder always sounds better* in a quick comparison, so you fool yourself into thinking a process "improved" the sound when it merely raised the level. Axon's two meters (in vs out) let you see exactly how much loudness you added, and the meter directly feeds the **Auto Gain** feature, which level-matches the output back to the input so you can compare tone and dynamics fairly (see §9).

---

## 3. The DSP behind it

### K-weighting filter (for LUFS)

BS.1770 prescribes a two-stage filter applied before the mean-square measurement:

1. A **"pre" / shelving filter** that boosts the high frequencies (~+4 dB shelf above ~1.5 kHz), modelling the acoustic effect of the head.
2. An **RLB high-pass** (Revised Low-frequency B-curve) that rolls off very low bass, so subsonic energy does not dominate the loudness reading.

Both are second-order **biquads**. Axon uses the exact libebur128 coefficients ([`meter.cpp:17-24`](../native/clap/src/meter.cpp)):

```cpp
constexpr KCoeffs k48{          // 48 kHz
    1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585,  // pre
    1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621,                                          // RLB
};
constexpr KCoeffs k44{          // 44.1 kHz
    1.5308412300503478, -2.6509799000031379, 1.1690790340624427, -1.6636551132560902, 0.7125954280732254,
    1.0, -2.0, 1.0, -1.9891696736297957, 0.9891959257876969,
};
```

These are exact for 44.1 and 48 kHz. For any other sample rate the code does a **proportional z-plane warp** from the 48 kHz coefficients ([`meter.cpp:42-48`](../native/clap/src/meter.cpp)) — an approximation, justified by the comment "good enough — LUFS integrates over seconds." The filter is selected/loaded in `set_k_weighting_` ([`meter.cpp:33-51`](../native/clap/src/meter.cpp)).

The same K-weighting constants are shared with the older **LufsLeveler** unit, which is why the meter can be cross-checked against it (see [`meter.hpp:4-6`](../native/clap/src/meter.hpp) and Related modules).

### Mean-square accumulation and gating windows

After K-weighting, each sample's filtered value is squared and **channel-summed** (L²+R² for stereo) ([`meter.cpp:92-95`](../native/clap/src/meter.cpp)). Energy is accumulated in **100 ms sub-blocks** (`kSubMs = 100`), which is the BS.1770 measurement granularity. A ring buffer of 30 sub-blocks covers the 3-second short-term window:

```cpp
static constexpr std::size_t kSubMs      = 100;
static constexpr std::size_t kShortMs    = 3000;  // 30 sub-blocks
static constexpr std::size_t kMomBlocks  = 4;     // 400 ms
```
([`meter.hpp:72-74`](../native/clap/src/meter.hpp))

- **Short-term LUFS** = mean of *all filled* sub-blocks (up to 30 → 3 s) ([`meter.cpp:114-115`](../native/clap/src/meter.cpp)).
- **Momentary LUFS** = mean of the *most recent 4* sub-blocks (400 ms) ([`meter.cpp:117-125`](../native/clap/src/meter.cpp)).

Note: this is a **plain sliding mean**, not the BS.1770 *gated* integration (which would discard sub-blocks below an absolute/relative threshold). That gating is only relevant to the integrated measurement, which this meter does not compute.

### LUFS conversion

The mean-square value is turned into LUFS with the BS.1770 formula ([`meter.cpp:26-29`](../native/clap/src/meter.cpp)):

```cpp
inline double lufs_from_ms(double ms) {
    if (ms <= 0.0) return -120.0;
    return -0.691 + 10.0 * std::log10(ms);
}
```

The constant **-0.691 dB** is the BS.1770 absolute-scale calibration offset; the `10·log10` (power, not amplitude) reflects that `ms` is already a *squared* quantity. A silent / zero input floors to **-120 dB**.

### RMS (unweighted)

RMS is measured separately, with **no** K-weighting. It uses the per-sample channel average `m = 0.5·(L+R)` ([`meter.cpp:104`](../native/clap/src/meter.cpp)), squared and averaged over a sliding window of **3 × 100 ms ≈ 300 ms** (`rms_n_ = 3`, [`meter.cpp:67-74`](../native/clap/src/meter.cpp)). Conversion is again power-domain `10·log10(mean)` ([`meter.cpp:137-138`](../native/clap/src/meter.cpp)), floored at -120 dB.

### Peak (decaying sample peak)

Peak tracks the absolute max sample across channels, with an exponential decay so the display falls back when the signal quiets ([`meter.cpp:97-101`](../native/clap/src/meter.cpp)):

```cpp
peak_lin_ *= peak_decay_per_sample_;
if (a > peak_lin_) peak_lin_ = a;
```

The decay constant is `exp(-1 / (0.375 · sr))` ([`meter.cpp:78`](../native/clap/src/meter.cpp)), a time constant τ ≈ 0.375 s, which the comment describes as "≈ 11.6 dB/s." Peak is converted to dBFS with the *amplitude* formula `20·log10(peak)` ([`meter.cpp:150-151`](../native/clap/src/meter.cpp)) — note `20·log10` for a linear amplitude vs `10·log10` for the squared mean-square quantities. This is a **sample peak**, not an oversampled true-peak.

---

## 4. How it works in the code

### The struct

`LoudnessMeter` ([`meter.hpp:22-99`](../native/clap/src/meter.hpp)) holds:

- two `Biquad`s, `pre_` and `rlb_`, with **per-channel state** (`z1l/z2l/z1r/z2r`) so L and R are filtered independently ([`meter.hpp:40-56`, `62-63`](../native/clap/src/meter.hpp));
- the K-weighted mean-square ring `ms_ring_` (30 slots) plus its running sum, index, fill count;
- a separate RMS ring `rms_ring_` (3 slots);
- the decaying `peak_lin_`;
- published doubles `lufs_s_`, `lufs_m_`, `rms_db_`.

The public `Readout` struct ([`meter.hpp:24-29`](../native/clap/src/meter.hpp)) carries the four floats out: `lufs_s`, `lufs_m`, `rms_db`, `peak_db`.

### `reset(sample_rate)` — [`meter.cpp:53-82`](../native/clap/src/meter.cpp)

The **only** place that allocates. It selects the K-weighting coefficients, sizes the sub-block length (`sub_len_ = 100 ms` in samples), `assign`s the two ring buffers (30 and 3 slots), computes the peak decay constant, and floors all published values to -120 dB. Called per stream activation/reset in the plugin ([`axon_plugin.cpp:1054-1055`, `1080-1081`](../native/clap/src/axon_plugin.cpp)).

### `process(L, R, n_ch, n)` — [`meter.cpp:84-143`](../native/clap/src/meter.cpp)

Called once per audio block. Buffers are **read-only** (`out` may alias `in`; `R` may be `nullptr` for mono — see [`meter.hpp:33-35`](../native/clap/src/meter.hpp)). Per sample it: K-weights and squares for LUFS, updates the decaying peak, and accumulates the unweighted RMS. On each 100 ms boundary it pushes a sub-block into the ring, updates the running sum incrementally (`ring_sum_ += ms - ms_ring_[idx]`), and recomputes short-term and momentary LUFS ([`meter.cpp:107-129`](../native/clap/src/meter.cpp)). RMS updates on its own 100 ms boundary ([`meter.cpp:131-141`](../native/clap/src/meter.cpp)). No heap allocation, no locks.

### `readout()` — [`meter.cpp:145-153`](../native/clap/src/meter.cpp)

A `const` snapshot of the current values, converting `peak_lin_` to dBFS on the fly. Cheap; can be called multiple times per block (the plugin calls it for both publishing and Auto Gain).

### Publishing to the UI

The plugin owns `meter_in` and `meter_out` ([`axon_plugin.cpp:699-700`](../native/clap/src/axon_plugin.cpp)) and a set of `std::atomic<float>` mirrors ([`axon_plugin.cpp:701-704`](../native/clap/src/axon_plugin.cpp)). After each block, the four-from-each readout values are `store`d with `std::memory_order_relaxed` ([`axon_plugin.cpp:1518-1528`](../native/clap/src/axon_plugin.cpp)). A timer callback (~21 fps, the same cadence as the spectrum) reads those atomics and pushes them to the WebView UI as an `axonMeters({...})` JSON call ([`axon_plugin.cpp:1860-1871`](../native/clap/src/axon_plugin.cpp)).

---

## 5. Latency & performance

- **Zero added latency.** The meter is a parallel *tap* — it reads the audio and produces numbers but never feeds anything back into the signal path. Removing it would not change a single output sample.
- **Real-time safe.** All allocation happens in `reset()`; `process()` does no allocation and takes no locks (confirmed by the header note, [`meter.hpp:8`](../native/clap/src/meter.hpp)). Per-sample cost is two biquads plus a few multiply-adds.
- **Lock-free hand-off.** The audio thread writes plain `std::atomic<float>` values with relaxed ordering; the UI thread reads them. Each value is independent, so a reader may briefly see a mix of two blocks' fields, which is harmless for a visual meter.

---

## 6. Parameters / fields

The meter has **no user-facing parameters** — it is purely observational. What it *exposes* per stream (`Readout`, [`meter.hpp:24-29`](../native/clap/src/meter.hpp)):

| Field      | Meaning                          | Window        | Conversion        | Floor   |
|------------|----------------------------------|---------------|-------------------|---------|
| `lufs_s`   | Short-term loudness (K-weighted) | 3 s (30×100ms)| `-0.691 + 10·log10(ms)` | -120 |
| `lufs_m`   | Momentary loudness (K-weighted)  | 400 ms (4×100ms)| same            | -120 |
| `rms_db`   | Unweighted RMS, dBFS             | ~300 ms       | `10·log10(mean)`  | -120 |
| `peak_db`  | Decaying sample peak, dBFS       | instantaneous + decay (τ≈0.375s) | `20·log10(peak)` | -120 |

Two instances exist: **`meter_in`** (raw input) and **`meter_out`** (real master).

---

## 7. Gotchas / things to watch

- **Momentary vs short-term.** `lufs_m` (400 ms) is jumpy and follows transients; `lufs_s` (3 s) is the steadier "section loudness." For hitting a streaming target, watch short-term. Auto Gain also drives off **short-term** (`lufs_s`).
- **Not gated, not "integrated."** This meter computes a sliding mean, not the BS.1770 *gated integrated* loudness of a whole program. Use it for live monitoring, not as a substitute for an offline integrated-LUFS render measurement. There is also no relative/absolute gate, so long silences pull the sliding mean down.
- **Sample peak, not true peak.** `peak_db` does not oversample, so it can under-read inter-sample peaks. The actual brickwall ceiling is enforced elsewhere (the TruePeakCeiling stage, see §9).
- **RMS is unweighted and uses the L/R *average*.** Out-of-phase stereo content can read lower than you might expect (cancellation in `0.5·(L+R)`); LUFS, which sums L²+R², does not have that issue.
- **Off-rate approximation.** At sample rates other than 44.1/48 kHz the K-weighting is a warped approximation, not the exact filter.
- **The OUT tap is *before* Auto Gain.** This is deliberate and the most important wiring detail — see below.

---

## 8. Where it sits in the Axon chain (in & out taps)

There are two taps:

- **IN tap** — at the very top of the chain, on the raw plugin input *before any processing* ([`axon_plugin.cpp:1192-1194`](../native/clap/src/axon_plugin.cpp), inside `flush_chain_block_`).
- **OUT tap** — after the full reorderable chain *and* after the fixed final stages **Trim → TruePeakCeiling**, i.e. on the **real master** ([`axon_plugin.cpp:1513-1516`](../native/clap/src/axon_plugin.cpp)).

The critical subtlety: the OUT meter is placed **before Auto Gain**. Auto Gain is a *monitoring-only* trim that quietens the output for fair A/B; if the meter sat after it, the OUT reading would always be dragged toward the input level and you could never see the true mastered loudness. By tapping the real master first, "driving the limiter harder" visibly raises the OUT LUFS, which is what you want when aiming at a target. The plugin comments spell this out:

> "This is the REAL master; the OUT meter reads it ... and Auto Gain is applied *after* metering." ([`axon_plugin.cpp:1499-1501`](../native/clap/src/axon_plugin.cpp))

```
[input] --IN tap--> [reorderable stages] -> Trim -> TruePeakCeiling --OUT tap--> [Auto Gain trim] -> [host out]
            |                                                              |
        meter_in                                                      meter_out
```

## 9. Related modules

- **Auto Gain** ([`native/clap/src/auto_gain.hpp`](../native/clap/src/auto_gain.hpp), wired at [`axon_plugin.cpp:1530-1549`](../native/clap/src/axon_plugin.cpp)). It is the meter's main *consumer*: it reads `meter_in.readout().lufs_s` and `meter_out.readout().lufs_s` and computes a trim that brings the delivered output down to the input's loudness for level-matched bypass. It also feed-forwards the limiter Drive (`drive_db × ml_wet`) so a Drive change is cancelled instantly instead of bumping for ~3 s while the LUFS loop catches up. Because Auto Gain is monitoring-only, you render with it **off** to print the loud master.
- **TruePeakCeiling / Trim** — the fixed final stages whose output *is* what the OUT meter measures.
- **LufsLeveler** (legacy) — shares the identical K-weighting constants ([`meter.hpp:4-6`](../native/clap/src/meter.hpp)), which is the basis for cross-checking the meter's LUFS numbers against the older unit.
