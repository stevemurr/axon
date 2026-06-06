# Auto Gain — Level-Matched Bypass ("Gain Match")

> A loudness-neutral monitoring trim that pulls the processed output down (or up) to match the *input* loudness, so you can A/B the chain honestly instead of being fooled by "louder = better."

---

## 1. What is this?

When you turn on a mastering plugin and it makes the sound a little louder, your ears almost always tell you it sounds *better* — richer, fuller, more "finished." This is a real, well-documented quirk of human hearing called the **loudness bias**: at the volumes we usually monitor at, a tiny increase in level (even a fraction of a decibel) is perceived as a quality improvement, regardless of whether the processing actually improved anything.

That creates a trap. A mastering chain (saturation, EQ, compression, limiting) almost always raises loudness as a side effect. So when you flip between "bypassed" (chain off) and "engaged" (chain on), you are not comparing *timbre and dynamics* — you are mostly comparing *two different volumes*, and the louder one wins by default. You can fool yourself into thinking a chain is great when all it did was turn things up.

**Level-matched bypass** (a.k.a. "gain match" or "auto gain") fixes this. It measures how much louder (or quieter) the processed signal is than the raw input, and applies the *opposite* trim to the output you hear — only to the monitoring level, not to the final render. Now when you A/B, both states sit at the **same loudness**, and any difference you hear is a *real* difference in tone, punch, or dynamics. That is an honest comparison.

**Analogy.** Imagine two slices of cake. One is on a plate angled slightly toward you so it looks bigger. You'll probably "prefer" it — not because it tastes better, but because it *looks* like more. Level matching is the act of putting both slices on identical plates at the same angle, so you judge them on flavor alone. Auto Gain is the plate-leveler for your ears.

In Axon, Auto Gain is paired with the internal **Bypass** button, and the two together give you a fair, instant before/after.

---

## 2. Why it matters in mastering

Mastering is the stage where decisions are subtle and the stakes are high — you are making half-a-decibel, half-a-dB-of-EQ kind of choices. Those decisions are *only* trustworthy if your comparison is level-matched:

- **You can trust your A/B.** Engage vs. bypass at matched loudness tells you what the chain *actually does to the sound*, not just how loud it makes it.
- **You avoid the "always-on improvement" illusion.** Without level matching, every processor seems to help, so you keep stacking processing you don't need.
- **You make better gain-staging and limiter decisions.** When the limiter's Drive is doing nothing but raising level, level-matched monitoring exposes that immediately — the tone stops changing once you've matched, so you can hear the point of diminishing returns.

Crucially, Auto Gain in Axon is **monitoring-only**. It changes what you *hear* while auditioning, but it does **not** change the master you print. The loud, finished master is what gets rendered; Auto Gain just lets you evaluate it fairly along the way.

---

## 3. The DSP behind it

The whole module lives in [`native/clap/src/auto_gain.hpp`](../native/clap/src/auto_gain.hpp) — about 70 lines. Here is the idea, built up from intuition.

### Loudness, measured in LUFS

Loudness is measured in **LUFS** (Loudness Units Full Scale) — a perceptual loudness scale standardized for broadcast/streaming (ITU-R BS.1770 K-weighting). Axon's meters report **short-term LUFS** (`lufs_s`), which is loudness integrated over a sliding **3-second** window (`kShortMs = 3000` in `meter.hpp:73`). Short-term is the right timescale here: it's stable enough not to jitter on transients, but responsive enough to follow a mix.

### The key trick: feed-forward, not feedback

The naive ("feedback") way to build auto gain would be: measure the loudness of *what you actually output* (after your trim), compare it to the input, and nudge the trim. But that's a closed loop — the thing you measure depends on the gain you applied, which depends on the measurement... loops like this can ring, overshoot, or oscillate, and they're fiddly to tune.

Axon uses **feed-forward** instead. It meters the **REAL, uncompensated output** — the true master *before* Auto Gain's trim is applied (see §5). Because that measurement is taken upstream of the trim, it is **independent of the gain Auto Gain chooses**. So the amount we *should* turn down by is simply:

```
target_offset_dB = in_lufs − out_lufs       (clamped to ±24 dB)
```

If the chain made things +6 dB louder, `out_lufs − in_lufs = +6`, so the target trim is −6 dB. There is no loop: the target doesn't move just because we moved the gain. (`auto_gain.hpp:53`)

### One-pole smoothing toward the target

We don't snap to the target instantly — that would zip the level around as the loudness reading wobbles. Instead the running gain `g_db_` is eased toward the target with a **one-pole smoother** (exponential glide), once per audio block:

```
g_db_ += (target − g_db_) * kSmooth        // kSmooth = 0.004  (auto_gain.hpp:54, :65)
```

This is the classic `y += (x − y) * α` low-pass. With `α = 0.004` per block and a block of `kBlockSize = 128` samples (`axon_plugin.cpp:80`):

- At 48 kHz, one block ≈ 128 / 48000 ≈ **2.67 ms**.
- Time constant τ ≈ block_time / α ≈ 2.67 ms / 0.004 ≈ **~0.67 s**.
- ~95% settled in ~3τ ≈ **~2 s**, comfortably tracking the 3-second LUFS window without chasing every wiggle.

### dB ↔ linear

Gain is tracked in **dB** (perceptually natural, and what loudness math works in), then converted to a **linear multiplier** to apply to samples:

```
return std::pow(10.f, g_db_ / 20.f);       // dB → linear  (auto_gain.hpp:57)
```

(`/20` because we're scaling amplitude, not power.)

### Guards

- **Silence gate.** It only adapts when both meters read above the floor `kFloor = −50 dBFS` (`auto_gain.hpp:52, :63`). Without this, a silent passage (LUFS pinned at the floor) would wind the gain up to its clamp.
- **Clamp.** The gain is bounded to `±kMaxDb = 24 dB` (`auto_gain.hpp:53, :56, :64`) so a pathological reading can't blast or mute monitoring.

### Feed-forwarding a *known* gain (the limiter Drive)

There's a second, faster feed-forward path. When you grab a knob whose effect on level is *exactly known* — notably the **Mel Limiter Drive** — Axon hands that gain change to Auto Gain as `ff_db` so it can be **cancelled instantly**, rather than waiting ~2 s for the slow LUFS loop to catch up (which would let the monitor level bump audibly on every knob move). Only the **per-call delta** of `ff_db` is injected:

```
g_db_ += -(ff_db - last_ff_db_);           // cancel the known change now (auto_gain.hpp:47–48)
```

The LUFS loop then quietly trims the small, nonlinear residual the limiter actually produced. The injection only shapes the *transient*; the loop still converges to `target` regardless.

---

## 4. How it works in the code

### The class (`auto_gain.hpp`)

State is two floats:

| Field | Meaning | Line |
|---|---|---|
| `g_db_` | current monitoring gain, in dB | `auto_gain.hpp:68` |
| `last_ff_db_` | previous fed-forward gain (to take deltas) | `auto_gain.hpp:69` |

`reset()` zeroes both (`auto_gain.hpp:25`).

The whole job happens in one method (`auto_gain.hpp:34`):

```cpp
float process(bool enabled, float in_lufs, float out_lufs, float ff_db);
```

- **`in_lufs`** — input short-term LUFS (the target loudness).
- **`out_lufs`** — short-term LUFS of the **REAL (uncompensated) output**.
- **`ff_db`** — a *known* output gain (limiter Drive × wet) fed forward; only its delta is injected.
- **returns** — the **linear** gain to multiply this block's samples by.

**When disabled** (`auto_gain.hpp:35–42`): it doesn't hard-jump to unity (that would click). It **relaxes** `g_db_` toward 0 with a per-block decay `kRelease = 0.995` (`auto_gain.hpp:38, :66`), snapping to exactly 0 once within 0.01 dB. It also keeps updating `last_ff_db_` so that re-enabling later doesn't suddenly inject all the Drive change that happened while it was off.

**When enabled** (`auto_gain.hpp:44–57`): inject the `ff_db` delta, then (if not silent) glide `g_db_` toward `in_lufs − out_lufs`, clamp, and convert to linear.

`gain_db()` (`auto_gain.hpp:60`) exposes the current dB gain for diagnostics/tests.

### Constants (`auto_gain.hpp:63–66`)

```cpp
static constexpr float kFloor   = -50.f;   // LUFS gate (dBFS-ish floor)
static constexpr float kMaxDb   = 24.f;    // clamp, ± dB
static constexpr float kSmooth  = 0.004f;  // per-block one-pole toward target
static constexpr float kRelease = 0.995f;  // per-block decay toward 0 when off
```

### The call site (`axon_plugin.cpp`)

The contract is the load-bearing part, and the comments in the plugin spell it out (`axon_plugin.cpp:1499–1543`):

1. **The real master is finished first.** The Trim and `TruePeakCeiling` (the brickwall) run last in the chain — this is the actual master (`axon_plugin.cpp:1499–1511`).

2. **The OUT meter taps that real master, *before* Auto Gain.** `meter_out.process(...)` runs on `out_buf` (`axon_plugin.cpp:1513–1516`), and both in/out meter readouts are published to the UI (`axon_plugin.cpp:1517–1528`). So the OUT meter always shows the *true* master loudness, even when you're driving the limiter hard — the comment at `axon_plugin.cpp:1500` notes "driving the limiter shows the actual target loudness."

3. **The limiter Drive is computed as a known feed-forward gain** (`axon_plugin.cpp:1538–1539`):

   ```cpp
   const float drive_db = 20.f * std::log10(std::max(1e-6f, amt.ml_drive_lin));
   const float ff_db    = drive_db * amt.ml_wet;   // scale by limiter wet/dry mix
   ```

4. **Auto Gain is called once per block** with the *real* metered LUFS and the feed-forward dB (`axon_plugin.cpp:1540–1543`):

   ```cpp
   const float ag = plug.auto_gain.process(amt.auto_gain_on,
                                           plug.meter_in.readout().lufs_s,
                                           plug.meter_out.readout().lufs_s,
                                           ff_db);
   ```

5. **The returned gain is applied to the delivered output, *after* the meter tap** (`axon_plugin.cpp:1544–1549`):

   ```cpp
   if (ag != 1.f) {
       for (uint32_t ch=0; ch<n_ch; ++ch) {
           float* ob = plug.chains[ch].out_buf.data();
           for (int i=0;i<kBlockSize;++i) ob[i] *= ag;
       }
   }
   ```

So the data flow is: **chain → Trim → Ceiling → (METER taps here) → Auto Gain trim → to host**. The meter sees the real master; only your monitoring path gets trimmed.

The input LUFS comes from `meter_in`, which is metered on the **raw plugin input** at the top of the block (`axon_plugin.cpp:1192–1194`).

`auto_gain.reset()` is called on activate and on reset (`axon_plugin.cpp:1053, :1079`), alongside the meters' resets.

### Tested behavior

`native/clap/tests/test_auto_gain.cpp` verifies convergence: a chain that adds +6 dB settles to **g ≈ −6 dB** (output back to input loudness), a chain that's 4 dB quieter settles to **g ≈ +4 dB**, both within 0.5 dB after enough iterations.

---

## 5. Latency & performance

- **Latency: zero.** Auto Gain is a per-block scalar multiply. It adds no delay, no look-ahead, and is not part of the plugin's reported latency.
- **Cost: negligible.** Once per 128-sample block: a couple of `log10`/`pow` calls plus the per-sample multiply of the output buffer. No allocations, no locks, no branches that depend on data in a hot inner loop.
- **Real-time safe.** No heap, no system calls, no FFTs — fine to run on the audio thread.

---

## 6. Parameters

Auto Gain is deliberately minimal — it has **no user-facing knobs** except an on/off toggle.

| Control | UI | Internal id | Meaning |
|---|---|---|---|
| **Auto Gain** | "AUTO GAIN" button in the meter panel | `AGN` | Enable/disable level-matched monitoring. `auto_gain_on = (agn >= 0.5f)` (`axon_plugin.cpp:1168`). |

The UI button is wired in `native/clap/ui/index.html` (`auto-gain-btn` → `AGN`, see lines 443, 1730), with the tooltip *"Match output loudness to input for fair A/B."*

**Smoothing time** is *not* a parameter — it's the fixed `kSmooth = 0.004` per block (~0.67 s τ, ~2 s to settle). This is intentional: the glide is tuned to track the 3-second short-term LUFS window without audible pumping, and exposing it would invite mis-tuning.

### Interaction with Bypass

There's a sibling button, **Bypass** (`BYP`, `bypass-btn`, tooltip *"Audition the raw input (level-aligned, no DAW bypass)"*). They are complementary tools for honest auditioning:

- **Bypass** swaps the *wet* output for the *raw input*, pulled from a delay FIFO so it stays **time-aligned** with the processed path (`axon_plugin.cpp:1649–1657`). You hear the unprocessed signal at the same moment in time you'd hear the processed one.
- **Auto Gain** makes the *processed* path sit at the same *loudness* as that raw input.

Used together you get a time-aligned **and** loudness-matched A/B: hit Bypass to hear "before," release it to hear "after," and Auto Gain ensures the two are the same volume so the only difference is the processing itself. They are independent toggles — neither disables the other.

---

## 7. Gotchas / things to watch

- **It's monitoring-only — it does *not* change the OUT meter or the printed master.** The gain is applied *after* the meter tap (`axon_plugin.cpp:1544–1549`), so the OUT LUFS reading still shows the **real, loud master**. If you render/print with Auto Gain on, you still get the loud master; Auto Gain never reduces the actual output level you bounce. (Comment: "render with Auto Gain off to print the loud master," `axon_plugin.cpp:1534`.) The OUT meter and your ears can legitimately disagree here — that's the design.
- **Don't trust the meter to tell you the monitor level.** With Auto Gain on, what you *hear* is quieter than what the OUT meter reads. That's the whole point, but it surprises people.
- **It needs signal.** Below `kFloor = −50` LUFS it freezes (won't adapt), so silence won't wind the gain to the ±24 dB clamp.
- **It's slow on purpose.** After a big mix change it takes ~2 s to re-settle (tracking the 3 s LUFS window). This is correct behavior, not a bug — fast adaptation would pump.
- **Knob moves don't bump the level (for known gains).** Moving the limiter **Drive** is fed forward (`ff_db`) and cancelled instantly (`auto_gain.hpp:47`), so the monitor level holds steady while the slow loop trims the residual. Knobs whose effect on loudness *isn't* known analytically (e.g. EQ, saturation, compression) aren't fed forward — for those you'll see the normal ~2 s glide as the LUFS loop catches up.
- **Re-enabling is clean.** While disabled, it keeps tracking `ff_db` (`auto_gain.hpp:40`) so flipping it back on doesn't dump in all the Drive change that happened while it was off.

---

## 8. Where it sits in the Axon chain / monitoring path

Auto Gain is **not** a chain stage — it's a final monitoring trim that lives outside the reorderable processing chain. The signal path per block:

```
raw input ──► meter_in (input LUFS) ──► [reorderable stages: AutoEQ, Saturator,
   SSL Comp, Mel Limiter, BassMono, ...] ──► Trim ──► TruePeakCeiling
                                                          │
                                          (this is the REAL master)
                                                          ▼
                                              meter_out  ── OUT LUFS  ─────────┐
                                                          │                    │
                                                          ▼                    ▼
                                            Auto Gain × g  (monitoring)   (meters/UI)
                                                          │
                                                          ▼
                                                   to host / your ears
```

- Input loudness: `meter_in` (raw input, `axon_plugin.cpp:1192`).
- Real master + output loudness: `Trim`/`Ceiling` then `meter_out` (`axon_plugin.cpp:1499–1516`).
- Monitoring trim: `auto_gain.process(...)` then `out_buf *= ag` (`axon_plugin.cpp:1540–1549`).

### Related modules

- **Loudness Meter** (`meter.hpp` / `meter.cpp`) — supplies the short-term LUFS (`lufs_s`, 3 s window) that Auto Gain feeds on, for both input (`meter_in`) and the real output (`meter_out`). Auto Gain is essentially a consumer of two meter readings. See `docs`/the meter memory note for the LUFS/RMS/Peak details.
- **Mel Limiter** (`mel_limiter.cpp`, control id `MLI`/`MLD`) — its **Drive** is the one knob whose gain is fed forward into Auto Gain (`ff_db = drive_db * ml_wet`, `axon_plugin.cpp:1538–1539`), so driving the limiter doesn't bump the monitor level. The OUT meter deliberately reads *after* the limiter so you can watch the true loudness climb as you drive it.
- **Bypass** (internal, `BYP`) — the time-aligned raw-input audition that pairs with Auto Gain for a fair, loudness-matched before/after (`axon_plugin.cpp:1649–1657`).
