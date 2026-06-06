# Bass Mono — the low-end mono-maker

**One-sentence summary:** Bass Mono collapses the stereo image to mono *below* a chosen cutoff frequency (default 250 Hz) while leaving everything above it perfectly intact, keeping the low end tight, centred, and translatable across vinyl, clubs, and mono systems — without narrowing the mids and highs.

---

## 1. What is this?

### Stereo, mono, and "width" — the plain-English version

A stereo recording is just **two signals**: a left channel (`L`) and a right channel (`R`), one for each ear / speaker.

- If `L` and `R` are *identical*, the sound appears to come from dead centre, between the speakers. We call this **mono**.
- If `L` and `R` *differ*, your brain hears those differences as **width** — instruments spread out across the space between (and beyond) the speakers.

The part of the signal that is the *same* in both channels is the **mid** (centre). The part that *differs* is the **side** (width). This way of looking at a stereo signal is called **mid/side (M/S)**:

```
mid  (M) = the stuff both speakers agree on   → centre image
side (S) = the disagreement between L and R   → stereo width
```

There is **no width without side content.** If the side is zero, `L` equals `R`, and the sound is mono.

### Why would you want the *bass* to be mono?

Low frequencies (kick drums, bass guitars, sub-bass synths) often carry a lot of stereo "side" energy — sometimes intentionally, sometimes as an accident of recording, sample libraries, or stereo effects. Wide bass causes real problems:

- **Vinyl:** A record's groove encodes left as one wall and right as the other. Out-of-phase low frequencies (lots of side energy in the bass) make the cutting stylus jump *vertically*, which can literally make the needle skip out of the groove. Mastering engineers mono the bass to keep records cuttable and loud.
- **Clubs & PA systems:** Many big sound systems sum the bass to a single mono subwoofer. If your bass lives in the side channel, a mono sub **cancels it** — your track loses its low end on exactly the systems where bass matters most.
- **Phase & punch:** When `L` and `R` low end are slightly out of phase, the centre (mono) sum gets weaker and "smeary." Collapsing bass to mono makes it phase-coherent: tighter, punchier, more solid.

**Analogy:** Think of the stereo field as two people carrying a heavy table (the bass). If they pull in slightly different directions (wide/out-of-phase bass), the table wobbles and they waste effort. Bass Mono makes them stand shoulder-to-shoulder and lift together — same load, all the force pointing the same way.

The key idea: this module removes **low-frequency width**, not low-frequency *level*. The bass is still there, just as loud — it's simply centred.

---

## 2. Why it matters in mastering

Mastering is the final polish before a track is released, and a huge part of the job is making the track sound good *everywhere*: earbuds, laptops, car stereos, club rigs, and vinyl. "Bass mono" is one of the most common, low-risk mastering moves because:

1. **Translation.** A track with mono bass behaves predictably on mono-sub systems and vinyl.
2. **Loudness.** Phase-coherent (mono) bass sums more efficiently, so the limiter downstream can push the track louder for the same peak level.
3. **Cleanliness.** It tightens a "flubby" or undefined low end without an EQ cut — you keep the energy, you just focus it.

Because the operation is surgical (only the *side* of the *low* band is affected), it's safe to apply on almost any master. That's why it sits as a dedicated stage in the Axon chain.

---

## 3. The DSP behind it

### Step 1 — Mid/Side decomposition

From `L` and `R` we compute the mid and side:

```
M = ½ (L + R)        // centre
S = ½ (L − R)        // width
```

This is exactly reversible. To get the channels back:

```
L = M + S
R = M − S
```

(Check: `M + S = ½(L+R) + ½(L−R) = L`. ✓)

In the code (`bass_mono.hpp:45–49`):

```cpp
const double m = 0.5 * (static_cast<double>(l[i]) + r[i]);
const double s = 0.5 * (static_cast<double>(l[i]) - r[i]);
const double sh = hp2_.process(hp1_.process(s));   // side, HPF'd
l[i] = static_cast<float>(m + sh);
r[i] = static_cast<float>(m - sh);
```

### Step 2 — High-pass the SIDE only

We run the side signal `S` through a **high-pass filter (HPF)**: a filter that *lets through* frequencies above the cutoff and *removes* frequencies below it. The result is `S_hp` (`sh` in the code).

- Below the cutoff, `S_hp → 0`, so `L = M + 0 = M` and `R = M − 0 = M`. **L = R = mono.**
- Above the cutoff, `S` passes untouched, so the full stereo width is preserved.

**The MID is never filtered.** That is the crucial design choice. Since `M = ½(L+R)`, the mono sum `L + R = 2M` is the same before and after processing — at *every* frequency. We only ever subtract width (side) energy from the bass; we never touch the actual mono content. So Bass Mono **cannot change the mono/centre tonal balance** — it only removes low-frequency width.

```
        ┌─────────────┐
  S ───▶│  HPF (LR4)  │───▶ S_hp     (side: bass width removed)
        └─────────────┘
  M ──────────────────────▶ M        (mid: untouched, mono sum preserved)
```

### Step 3 — Why a 4th-order Linkwitz-Riley filter

How *steeply* a filter rolls off is measured in **dB per octave** (an octave is a doubling of frequency). A gentle filter bleeds some bass width through just below the cutoff; a steep filter makes a cleaner split.

Bass Mono uses a **4th-order Linkwitz-Riley high-pass = 24 dB/octave** (`bass_mono.hpp:11–12, 82`). An LR4 filter is built by **cascading two identical 2nd-order Butterworth filters** (each is 12 dB/oct; two in series = 24 dB/oct):

```
  S ──▶ [Butterworth HPF #1] ──▶ [Butterworth HPF #2] ──▶ S_hp
            (hp1_, 12 dB/oct)        (hp2_, 12 dB/oct)
                              = LR4, 24 dB/oct
```

A defining property of LR alignment: at the cutoff frequency each Butterworth section is at −3 dB, so two cascaded sections give **−6 dB**, and the filter's phase behaves cleanly. For *this* application the steep, predictable slope is what we want: a tight, clean transition from "mono bass" to "full-width" with no lingering side energy creeping in below the cutoff.

### The biquad and its coefficients

Each 2nd-order section is implemented as a **biquad** — the workhorse building block of digital IIR filters. Its difference equation (Transposed Direct Form II, as used here) is:

```
y[n] = b0·x[n] + z1
z1   = b1·x[n] − a1·y[n] + z2
z2   = b2·x[n] − a2·y[n]
```

This appears verbatim at `bass_mono.hpp:57–62`. The `z1`/`z2` are the two **state variables** (the filter's memory of past samples) — this is what makes it an IIR (Infinite Impulse Response) filter and what gives it near-zero latency.

The coefficients come from the **RBJ Audio EQ Cookbook** formula for a Butterworth high-pass with **Q = 1/√2 ≈ 0.70710678** (`bass_mono.hpp:69–84`). Given cutoff `fc` and sample rate `sr`:

```
w0    = 2π · fc / sr
cw    = cos(w0)
sw    = sin(w0)
Q     = 0.70710678            // Butterworth Q for a maximally flat passband
alpha = sw / (2·Q)
a0    = 1 + alpha

b0 =  (1 + cw)/2 / a0
b1 = −(1 + cw)   / a0
b2 =  (1 + cw)/2 / a0
a1 = −2·cw       / a0
a2 =  (1 − alpha)/ a0
```

Note that everything is pre-divided by `a0`, so the difference equation can assume a normalized `a0 = 1`. **Both** biquads (`hp1_` and `hp2_`) are loaded with these *same* coefficients (`bass_mono.hpp:83–84`) — that's precisely what makes the cascade a Linkwitz-Riley alignment rather than two arbitrary filters.

The cutoff is also sanity-clamped before use (`bass_mono.hpp:71`):

```cpp
const double fc = (cutoff_ > 1.0 && cutoff_ < 0.49 * sr_) ? cutoff_ : 250.0;
```

If the requested cutoff is non-physical (≤ 1 Hz, or above ~49% of the sample rate — i.e. near Nyquist where a digital filter misbehaves), it falls back to the safe default of **250 Hz** rather than producing garbage coefficients.

---

## 4. How it works in the code

File: `native/clap/src/bass_mono.hpp` (class `nablafx::BassMono`).

### `prepare(double sample_rate)` — `bass_mono.hpp:26–30`

Called once when the host tells the plugin the sample rate (and at every restart). It stores `sr_`, clears filter state via `reset()`, then calls `design_()` to compute coefficients for the current cutoff. After `prepare`, the module is ready to process.

In the plugin this is wired at `axon_plugin.cpp:1052`:

```cpp
plug->bass_mono.prepare(plug->sample_rate);
```

### `set_cutoff(float hz)` — `bass_mono.hpp:33–37`

```cpp
void set_cutoff(float hz) {
    if (hz == cutoff_) return;   // no change → no work
    cutoff_ = hz;
    design_();                   // recompute the 5 coefficients
}
```

Note the **change-detection guard**: coefficients are only recomputed when the cutoff actually moves. This avoids running the (relatively expensive) `sin`/`cos` math every audio block when the knob isn't moving — important for real-time efficiency. It does *not* clear the filter state, so a moving cutoff retunes smoothly without clicking.

### `reset()` — `bass_mono.hpp:39`

Clears both biquads' state (`hp1_.clear(); hp2_.clear();`), zeroing `z1`/`z2`. Called on transport reset / activation (`axon_plugin.cpp:1078`) so stale samples from a previous playback position don't leak into the new one.

### `process(float* l, float* r, int n)` — `bass_mono.hpp:43–51`

The audio loop, shown in full in §3. Key points:

- **In-place stereo:** it reads and writes the same `l[]` and `r[]` buffers — no allocation, no copying inside the module.
- **`double` internal precision:** samples are promoted to `double` for the M/S math and the IIR recursion (`bass_mono.hpp:45–47`), then cast back to `float` on output. IIR filters accumulate error over time, so the extra precision keeps the low-frequency math clean.
- **Sequential cascade:** `hp2_.process(hp1_.process(s))` — the side goes through filter 1, then its output through filter 2.

### Plugin wiring — `axon_plugin.cpp:1476–1487`

The stage is dispatched in the per-block stage loop:

```cpp
case StageID::BassMono: {
    if (amt.bm_wet <= 0.f || n_ch < 2) break;          // off, or mono input → skip
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

What the *plugin* adds on top of the bare DSP class:

- **Early-out** when the effect amount (`bm_wet`) is zero, or when the input isn't actually stereo (`n_ch < 2`). On a mono track there's no side to remove, so it's a clean no-op.
- **Wet/dry blend.** The DSP itself is fully "wet." To let the user dial the effect *partially* in, the plugin processes a *copy* (`bl`/`br`) and then mixes it back toward the dry signal with `blend_inplace_` (`axon_plugin.cpp:1177–1179`):

  ```cpp
  static void blend_inplace_(float* buf, const float* wet, float w, int n) {
      if (w>=1.f) { std::copy_n(wet,n,buf); return; }
      for (int i=0;i<n;++i) buf[i]+=(wet[i]-buf[i])*w;   // buf = buf + w·(wet−buf)
  }
  ```

  So `bm_wet = 1.0` is full bass-mono, `0.5` is half-strength, `0.0` is bypassed.

The control values are resolved into the `AmountSnapshot` at `axon_plugin.cpp:1166–1167` from the `"BMI"` and `"BMF"` controls (`axon_plugin.cpp:1146`).

---

## 5. Latency & performance

- **Latency: effectively zero.** Bass Mono is an **IIR** filter. Unlike an FIR/linear-phase filter (which delays the signal by half its length), an IIR biquad produces output on the *same* sample it receives input — there is no group delay budget to report to the host. The module reports/requires no PDC (plug-in delay compensation). It *does* introduce frequency-dependent phase shift in the side channel near the cutoff (inherent to any minimum-phase IIR), but no bulk latency.
- **Real-time safe.** `process()` allocates nothing and contains no locks, no system calls — just arithmetic over the buffer. It is safe to call on the audio thread.
- **Cheap.** Per stereo sample: a couple of adds for M/S, two biquads (≈5 mul + 4 add each) on the side, and two adds to recombine. Coefficient design (`sin`/`cos`) only runs when the cutoff changes, not per sample (see the guard in `set_cutoff`).
- The plugin-side copy + blend (`axon_plugin.cpp:1480–1485`) adds one buffer copy and one blend pass per block — negligible, and only when the stage is active.

---

## 6. Parameters

Defined in `weights/axon_bundle/axon_meta.json` and surfaced in the UI as the **BASS MONO** stage (`native/clap/ui/index.html:459`).

| Control | ID | Name | Range | Default | Unit | Meaning |
|---|---|---|---|---|---|---|
| Amount / enable | `BMI` | "Bass Mono" | 0.0 – 1.0 | **0.0** (off) | switch / wet | Wet/dry mix of the effect. `0` = bypassed, `1` = fully mono'd below cutoff. Treated as a switch in the UI; internally a continuous blend (`bm_wet`). |
| Frequency | `BMF` | "Frequency" | 20 – 500 Hz | **250 Hz** | Hz | The cutoff. Stereo width below this is collapsed to mono; above it is untouched (`bm_freq` → `set_cutoff`). |

Notes:

- The module's *internal* default cutoff (`cutoff_ = 250.f`, `bass_mono.hpp:88`) matches the `BMF` default of 250 Hz, which is also the clamp fallback (`bass_mono.hpp:71`).
- `BMI` defaults to **0.0**, so Bass Mono is **off by default** — it does nothing until the user enables it.
- Typical mastering cutoffs land around **100–250 Hz**; the 20–500 Hz range covers everything from "sub-only" mono to a fairly aggressive low-mid mono.

---

## 7. Gotchas / things to watch

- **It removes width, not level.** Bass Mono never changes the mono sum (`L+R`), so on a mono playback system the track sounds *identical* with the effect on or off. If you A/B in mono to check "how much bass it removed," you'll hear no difference — that's by design. Listen in **stereo** (or check a correlation/goniometer meter) to hear the effect.
- **It does nothing on mono sources.** With `n_ch < 2` the stage early-outs (`axon_plugin.cpp:1477`). No side channel, nothing to collapse.
- **Phase shift near the cutoff.** Being a minimum-phase IIR, the *side* channel picks up some phase rotation around `BMF`. This is normal and inaudible in practice, but vets should know it's not a linear-phase split — the mid is phase-clean (untouched), the side is not.
- **Transients.** Wide bass transients (e.g. a stereo-effected kick tail) will get centred. This is usually the goal, but if a sound *relies* on its low-frequency width as an effect, monoing it will tighten/narrow it. Use a lower `BMF` if you only want to mono true sub.
- **Cutoff fallback is silent.** Asking for a cutoff ≤ 1 Hz or near Nyquist silently snaps to 250 Hz (`bass_mono.hpp:71`) rather than erroring. The exposed `BMF` range (20–500 Hz) keeps you well inside the valid zone, so you won't normally trip this.
- **The DSP class is all-wet.** Partial-strength bass-mono comes entirely from the plugin's `blend_inplace_` wrapper, not the `BassMono` class. If you reuse the class elsewhere, you get 100% wet.

---

## 8. Where it sits in the Axon chain

Bass Mono is **`StageID::BassMono = 6`** (`axon_plugin.cpp:103`). In the default processing order it runs late, just before the final limiter (`axon_plugin.cpp:666`):

```
AutoEQ → Saturator → SslComp → BassMono → MelLimiter
```

```mermaid
flowchart LR
    A[AutoEQ\nneural tonal match] --> B[Saturator\nRationalA]
    B --> C[SslComp\nglue compressor]
    C --> D[BassMono\nlow-end mono-maker]
    D --> E[MelLimiter\nbrickwall / loudness]
```

Placing Bass Mono **before** the limiter is deliberate: by centring and phase-aligning the bass first, the low end sums more efficiently, so the MelLimiter can achieve more loudness for the same peak ceiling. (The Axon chain is reorderable, but this is the shipped default.)

## Related modules

- **MelLimiter** (`mel_limiter.hpp`) — the next/final stage; benefits from the phase-coherent bass Bass Mono produces.
- **SslComp** (`ort(ssl_comp)`) — the glue compressor immediately upstream.
- **AutoEQ / Saturator (RationalA)** — earlier tonal stages.
- **Stage system** — Bass Mono was added as a reorderable stage (`StageID 6`); see the project's stage-system notes for how stages are registered, ordered, and dispatched.
- **Unit tests:** `native/clap/tests/test_bass_mono.cpp` verifies that low-frequency width is removed while the mono sum is preserved.
